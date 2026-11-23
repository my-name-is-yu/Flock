#include "flock/markdown.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace flockv0;

namespace {
void check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void issueExample() {
    const std::string source = "**完了**：[結果を見る](https://example.com/result)";
    check(markdown::toHtml(source) == "<p><strong>完了</strong>：<a href=\"https://example.com/result\">結果を見る</a></p>",
          "Web rendering changed the Markdown example");
    const auto slack = markdown::toSlack(source);
    check(slack.mrkdwn && slack.text == "*完了*：<https://example.com/result|結果を見る>",
          "Slack rendering changed the Markdown example");
    check(markdown::toPlain(source) == "完了：結果を見る（https://example.com/result）",
          "plain rendering changed the Markdown example");
}

void slackQueryUrl() {
    const auto slack = markdown::toSlack("[search](https://example.com/search?q=a&lang=en)");
    check(slack.mrkdwn && slack.text == "<https://example.com/search?q=a&amp;lang=en|search>",
          "Slack link escaping changed a query URL");
}

void slackCodeDelimiters() {
    const std::string inlineSource =
        "Before **bold** and ``literal ` ** _ <tag> >`` after [search](https://example.com/search?q=a&lang=en)";
    const std::string inlineExpected =
        "Before bold and literal ` ** _ &lt;tag&gt; &gt; after search (https://example.com/search?q=a&amp;lang=en)";
    const auto inlineReply = markdown::toSlack(inlineSource);
    check(!inlineReply.mrkdwn && inlineReply.text == inlineExpected,
          "Inline code with a backtick did not fall back to escaped plain text");

    const std::string fencedSource =
        "[search](https://example.com/search?q=a&lang=en)\n\nBefore **bold**\n\n````text\n**stars** _under_ <tag> > ```\n````";
    const std::string fencedExpected =
        "search (https://example.com/search?q=a&amp;lang=en)\n\nBefore bold\n\n**stars** _under_ &lt;tag&gt; &gt; ```\n";
    const auto fencedReply = markdown::toSlack(fencedSource);
    check(!fencedReply.mrkdwn && fencedReply.text == fencedExpected,
          "Fenced code with triple backticks did not fall back to escaped plain text");
}

void slackTextFallback() {
    const auto commonmarkText = markdown::toSlack("**bold** with ~literal~ and [docs](https://example.com/search?q=a&lang=en)");
    check(!commonmarkText.mrkdwn &&
          commonmarkText.text == "bold with ~literal~ and docs (https://example.com/search?q=a&amp;lang=en)",
          "Slack reinterpreted literal CommonMark text or failed to expand a link in fallback mode");

    const auto escapedPunctuation = markdown::toSlack("Escaped \\*literal\\* and \\_under\\_");
    check(!escapedPunctuation.mrkdwn && escapedPunctuation.text == "Escaped *literal* and _under_",
          "Slack reinterpreted escaped Markdown punctuation");

    const auto htmlInline = markdown::toSlack("<span data-format=\"*\">literal</span>");
    check(!htmlInline.mrkdwn && htmlInline.text == "&lt;span data-format=\"*\"&gt;literal&lt;/span&gt;",
          "Slack reinterpreted a formatting character in inline HTML");

    const auto htmlBlock = markdown::toSlack("<div>*literal*</div>\n");
    check(!htmlBlock.mrkdwn && htmlBlock.text == "&lt;div&gt;*literal*&lt;/div&gt;\n",
          "Slack reinterpreted a formatting character in block HTML");
}

void codeAndNewlines() {
    const std::string source = "```text\n**use** _literal_ <tag> > value\nline two\n```";
    const std::string literal = "**use** _literal_ <tag> > value\nline two\n";
    check(markdown::toPlain(source) == literal,"plain rendering changed fenced code content");
    check(markdown::toHtml(source) == "<pre><code>**use** _literal_ &lt;tag&gt; &gt; value\nline two\n</code></pre>",
          "Web rendering changed fenced code content");
    const auto slack = markdown::toSlack(source);
    check(slack.mrkdwn && slack.text == "```\n**use** _literal_ &lt;tag&gt; &gt; value\nline two\n```",
          "Slack rendering changed fenced code content or exposed Slack syntax");
    const std::string paragraphs = "first line\nsecond line\n\nthird paragraph";
    check(markdown::toPlain(paragraphs) == "first line\nsecond line\n\nthird paragraph",
          "plain rendering changed paragraph or soft-break newlines");
    const auto emptySlack = markdown::toSlack("");
    check(markdown::toHtml("").empty() && emptySlack.text.empty() && emptySlack.mrkdwn && markdown::toPlain("").empty(),
          "empty Markdown produced output");
    check(markdown::toHtml("`multiple  spaces`") == "<p><code>multiple  spaces</code></p>" &&
          markdown::toPlain("`multiple  spaces`") == "multiple  spaces",
          "inline code content collapsed repeated spaces");
}

void linksAndRawHtml() {
    const std::string unsafe = "[scheme](javascript:alert%281%29) [entity](java&#x73;cript:alert%281%29) [encoded](https%3A%2F%2Fevil.example) [relative](//evil.example/path)";
    const auto html = markdown::toHtml(unsafe);
    const auto slackReply = markdown::toSlack(unsafe);
    const auto& slack = slackReply.text;
    check(html.find("<a href=") == std::string::npos && html.find("javascript:") != std::string::npos &&
          html.find("https%3A%2F%2Fevil.example") != std::string::npos,
          "unsafe or encoded destinations became Web links or were hidden");
    check(slackReply.mrkdwn && slack.find("<javascript:") == std::string::npos && slack.find("<https%3A") == std::string::npos &&
          slack.find("javascript:") != std::string::npos,
          "unsafe or encoded destinations became Slack links or were hidden");

    const std::string rawHtml = "Before <script>notify()</script> after";
    const auto safeHtml = markdown::toHtml(rawHtml);
    check(safeHtml == "<p>Before &lt;script&gt;notify()&lt;/script&gt; after</p>",
          "raw HTML was not rendered as literal text");
    const auto slackHtml = markdown::toSlack(rawHtml);
    check(slackHtml.mrkdwn && slackHtml.text == "Before &lt;script&gt;notify()&lt;/script&gt; after",
          "raw HTML was not rendered as literal Slack text");
    check(markdown::toPlain(rawHtml) == rawHtml,"plain rendering changed raw literal text");

    const auto remoteImage = markdown::toHtml("![diagram](https://images.example/diagram.png)");
    check(remoteImage.find("<img") == std::string::npos && remoteImage.find("diagram") != std::string::npos &&
          remoteImage.find("https://images.example/diagram.png") != std::string::npos,
          "remote image was loaded or its fallback destination was hidden");
}

void nestedAndStable() {
    const std::string source = "## Title\n\n- one **bold _nested_**\n- [two](https://example.org)\n  - third `code`";
    const auto html = markdown::toHtml(source);
    const auto slackReply = markdown::toSlack(source);
    const auto& slack = slackReply.text;
    const auto plain = markdown::toPlain(source);
    check(html.find("<h2>Title</h2>") != std::string::npos &&
          html.find("<strong>bold <em>nested</em></strong>") != std::string::npos &&
          html.find("<ul>") != std::string::npos && html.find("<code>code</code>") != std::string::npos,
          "Web rendering lost nested formatting or lists");
    check(slackReply.mrkdwn && slack.find("*bold _nested_*") != std::string::npos &&
          slack.find("<https://example.org|two>") != std::string::npos &&
          slack.find("  • third `code`") != std::string::npos,
          "Slack rendering lost nested formatting or lists");
    check(plain.find("Title") != std::string::npos && plain.find("bold nested") != std::string::npos &&
          plain.find("two（https://example.org）") != std::string::npos && plain.find("third code") != std::string::npos,
          "plain rendering lost nested formatting or lists");
    const auto repeatedSlack = markdown::toSlack(source);
    check(markdown::toHtml(source) == html && repeatedSlack.text == slack && repeatedSlack.mrkdwn == slackReply.mrkdwn &&
          markdown::toPlain(source) == plain,
          "repeated rendering changed output");

    const std::string mentions = "Notify <!here>, <@U123>, <#C123>, @channel, #general & <tag>";
    const auto slackMentions = markdown::toSlack(mentions);
    check(slackMentions.mrkdwn && slackMentions.text == "Notify &lt;!here&gt;, &lt;@U123&gt;, &lt;#C123&gt;, @channel, #general &amp; &lt;tag&gt;",
          "Slack rendered literal notification syntax as active markup");
    check(markdown::toPlain("~~literal~~") == "~~literal~~","unsupported Markdown content was dropped");
}
}

int main() {
    try {
        issueExample();
        slackQueryUrl();
        slackCodeDelimiters();
        slackTextFallback();
        codeAndNewlines();
        linksAndRawHtml();
        nestedAndStable();
        std::cout << "Markdown rendering behavior passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
