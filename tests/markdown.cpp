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
    check(slack.mrkdwn && slack.text == "*完了*：<https://example.com/result|結果を見る>", "Slack rendering changed the Markdown example");
    check(markdown::toPlain(source) == "完了：結果を見る（https://example.com/result）", "plain rendering changed the Markdown example");
}

void slackQueryUrl() {
    const auto slack = markdown::toSlack("[search](https://example.com/search?q=a&lang=en)");
    check(slack.mrkdwn && slack.text == "<https://example.com/search?q=a&amp;lang=en|search>", "Slack link escaping changed a query URL");
}

void slackCodeDelimiters() {
    const std::string inlineSource =
        "Before **bold** and ``literal ` ** _ <tag> >`` after [search](https://example.com/search?q=a&lang=en)";
    const std::string inlineExpected =
        "Before bold and literal ` ** _ &lt;tag&gt; &gt; after search (https://example.com/search?q=a&amp;lang=en)";
    const auto inlineReply = markdown::toSlack(inlineSource);
    check(!inlineReply.mrkdwn && inlineReply.text == inlineExpected, "Inline code with a backtick did not fall back to escaped plain text");

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
    check(!commonmarkText.mrkdwn && commonmarkText.text == "bold with ~literal~ and docs (https://example.com/search?q=a&amp;lang=en)",
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
    check(markdown::toPlain(source) == literal, "plain rendering changed fenced code content");
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

void definitionOnlySource() {
    const std::string source = "[ref]: https://example.com/?q=a&b=c \"title <tag> * _ ~ `\"";
    const std::string escaped = "[ref]: https://example.com/?q=a&amp;b=c \"title &lt;tag&gt; * _ ~ `\"";
    const auto slack = markdown::toSlack(source);
    check(markdown::toHtml(source) == escaped && slack.text == escaped && !slack.mrkdwn && markdown::toPlain(source) == source,
          "definition-only Markdown did not preserve its source as safe literal text");

    const std::string whitespace = " \t\r\n ";
    const auto whitespaceSlack = markdown::toSlack(whitespace);
    check(markdown::toHtml(whitespace).empty() && whitespaceSlack.text.empty() && whitespaceSlack.mrkdwn &&
              markdown::toPlain(whitespace).empty(),
          "whitespace-only Markdown behavior changed");
}

void linksAndRawHtml() {
    const std::string unsafe =
        "[scheme](javascript:alert%281%29) [entity](java&#x73;cript:alert%281%29) [encoded](https%3A%2F%2Fevil.example) [relative](//evil.example/path)";
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
    check(safeHtml == "<p>Before &lt;script&gt;notify()&lt;/script&gt; after</p>", "raw HTML was not rendered as literal text");
    const auto slackHtml = markdown::toSlack(rawHtml);
    check(slackHtml.mrkdwn && slackHtml.text == "Before &lt;script&gt;notify()&lt;/script&gt; after",
          "raw HTML was not rendered as literal Slack text");
    check(markdown::toPlain(rawHtml) == rawHtml, "plain rendering changed raw literal text");

    const auto remoteImage = markdown::toHtml("![diagram](https://images.example/diagram.png)");
    check(remoteImage == "<p>diagram (<a href=\"https://images.example/diagram.png\">https://images.example/diagram.png</a>)</p>",
          "remote image was loaded or its fallback destination was hidden");
    const auto remoteSlack = markdown::toSlack("![diagram](https://images.example/diagram.png)");
    check(remoteSlack.mrkdwn && remoteSlack.text == "diagram (<https://images.example/diagram.png>)" &&
              markdown::toPlain("![diagram](https://images.example/diagram.png)") == "diagram（https://images.example/diagram.png）",
          "standalone image fallback changed");
}

void linkedImages() {
    const std::string linked = "[![diagram](https://images.example/diagram.png?q=a&lang=en)](https://example.com/outer)";
    const auto html = markdown::toHtml(linked);
    const auto slack = markdown::toSlack(linked);
    const auto outerAnchor = html.find("<a href=");
    check(html == "<p><a href=\"https://example.com/outer\">diagram (https://images.example/diagram.png?q=a&amp;lang=en)</a></p>" &&
              outerAnchor != std::string::npos && html.find("<a href=", outerAnchor + 1) == std::string::npos,
          "linked image produced nested HTML anchors");
    check(slack.mrkdwn && slack.text == "<https://example.com/outer|diagram (https://images.example/diagram.png?q=a&amp;lang=en)>",
          "linked image produced nested Slack links");
    check(markdown::toPlain(linked) == "diagram（https://images.example/diagram.png?q=a&lang=en）（https://example.com/outer）",
          "linked image plain text lost a destination");

    const std::string unsafeImage = "[![**bold _alt_**](javascript:alert%281%29)](https://example.com/outer)";
    const auto unsafeHtml = markdown::toHtml(unsafeImage);
    const auto unsafeSlack = markdown::toSlack(unsafeImage);
    check(unsafeHtml == "<p><a href=\"https://example.com/outer\"><strong>bold <em>alt</em></strong> (javascript:alert%281%29)</a></p>" &&
              unsafeHtml.find("href=\"javascript:") == std::string::npos,
          "unsafe linked-image destination became an HTML link or formatted alt was lost");
    check(unsafeSlack.mrkdwn && unsafeSlack.text == "<https://example.com/outer|*bold _alt_* (javascript:alert%281%29)>" &&
              unsafeSlack.text.find("<javascript:") == std::string::npos,
          "unsafe linked-image destination became a Slack link or formatted alt was lost");
    check(markdown::toPlain(unsafeImage) == "bold alt（javascript:alert%281%29）（https://example.com/outer）",
          "unsafe linked-image plain text lost its destinations");
}

void protectedHtml() {
    const auto ampersand = [](std::string_view text) { return text.find("A&B") != text.npos; };
    const auto adjacent = [](std::string_view text) { return text.find("AB") != text.npos; };
    const auto formatted = [](std::string_view text) { return text.find("AxB") != text.npos; };
    const auto space = [](std::string_view text) { return text.find("A B") != text.npos; };
    const auto literalPlus = [](std::string_view text) { return text.find("A+B") != text.npos; };
    const auto lines = [](std::string_view text) { return text == "A\nB"; };
    const auto redacted = "<p>[redacted]</p>";

    check(markdown::toHtml("", ampersand).empty(), "empty HTML changed when sensitive-text detection was enabled");
    const auto entityHtml = markdown::toHtml("A&amp;B", ampersand);
    const auto formattedHtml = markdown::toHtml("A**x**B", formatted);
    check(entityHtml == redacted && formattedHtml == redacted && markdown::toHtml("A**&**B", ampersand) == "<p>A**&amp;**B</p>",
          "HTML text entity or valid inline formatting bypassed sensitive-text detection");
    check(markdown::toHtml("[A](https://example.com)B", adjacent) == redacted,
          "HTML text split by a link bypassed sensitive-text detection");
    check(markdown::toHtml("A\nB", lines) == redacted && markdown::toHtml("A  \nB", lines) == redacted &&
              markdown::toHtml("A\n\nB", lines) == redacted && markdown::toHtml("- A\n- B", adjacent) == redacted,
          "HTML textContent did not preserve soft-break, paragraph, or list boundaries");
    const auto visibleMarkup = [](std::string_view text) { return text == "> A" || text == "---"; };
    check(markdown::toHtml("> A", visibleMarkup) == "<blockquote><p>A</p></blockquote>" && markdown::toHtml("---", visibleMarkup) == "<hr>",
          "block quote markers or thematic breaks were included in HTML textContent");
    check(markdown::toHtml("[credential](https://example.com/A%26B)", ampersand) == redacted &&
              markdown::toHtml("[credential](https://example.com/?q=A&amp;B)", ampersand) == redacted &&
              markdown::toHtml("[credential](https://example.com/?q=A+B)", space) == redacted,
          "encoded link credentials bypassed URL-sensitive detection");
    const auto hashAndSpace = [](std::string_view text) { return text.find("A B#C D") != text.npos; };
    check(markdown::toHtml("[credential](https://example.com/?q=%23A+B)", space) == redacted &&
              markdown::toHtml("[credential](https://example.com/?q=%23A+B%23C+D)", hashAndSpace) == redacted,
          "encoded query hashes changed original query boundaries before form decoding");
    check(markdown::toHtml("[docs](https://example.com/?q=ok#A+B)", space) == "<p><a href=\"https://example.com/?q=ok#A+B\">docs</a></p>",
          "a fragment plus was incorrectly interpreted as a query-form space");
    const auto nestedUrl = [](std::string credential, unsigned layers) {
        for (unsigned layer = 0; layer < layers; ++layer) {
            std::string encoded;
            for (const char ch : credential) {
                if (ch == '%')
                    encoded += "%25";
                else if (ch == '&')
                    encoded += "%26";
                else if (ch == '/')
                    encoded += "%2F";
                else
                    encoded += ch;
            }
            credential = encoded;
        }
        return "[credential](https://example.com/?token=" + credential + ")";
    };
    check(markdown::toHtml("[credential](https://example.com/?token=A%2526B)", ampersand) == redacted &&
              markdown::toHtml(nestedUrl("A&B", 4), ampersand) == redacted &&
              markdown::toHtml(nestedUrl("safe/value", 5), ampersand) == redacted,
          "nested URL percent encoding was missed or did not fail closed at the decoding bound");
    const auto afterNull = "[docs](https://example.com/?q=%00safe%252525252Fvalue)";
    check(markdown::toHtml(afterNull) == redacted && markdown::toHtml(afterNull, ampersand) == redacted,
          "a decoded NUL hid the remaining bytes from the URL decoding bound");
    check(markdown::toHtml("[docs](https://example.com/?q=%00A%2526B)", ampersand) == redacted,
          "the sensitive-text check did not decode the whole URL");
    check(markdown::toHtml("[credential](https://example.com/?token=A+B)", literalPlus) == redacted &&
              markdown::toHtml("[credential](https://example.com/?token=A%2BB)", literalPlus) == redacted &&
              markdown::toHtml("[credential](https://example.com/?token=A%2BB)", space) == redacted,
          "query-form plus interpretation lost literal-plus credentials or skipped a decoded stage");

    const auto imageFallback = [](std::string_view text) { return text == "diagram (https://images.example/diagram.png)"; };
    const auto unsafeFallback = [](std::string_view text) { return text == "label (javascript:credential)"; };
    const auto codeLiteral = [](std::string_view text) { return text.find("code-secret") != text.npos; };
    const auto codeBlockLiteral = [](std::string_view text) { return text.find("block-secret") != text.npos; };
    const auto htmlLiteral = [](std::string_view text) { return text.find("<span>html-secret</span>") != text.npos; };
    check(markdown::toHtml("![diagram](https://images.example/diagram.png)", imageFallback) == redacted &&
              markdown::toHtml("[label](javascript:credential)", unsafeFallback) == redacted &&
              markdown::toHtml("`code-secret`", codeLiteral) == redacted &&
              markdown::toHtml("```\nblock-secret\n```", codeBlockLiteral) == redacted &&
              markdown::toHtml("<span>html-secret</span>", htmlLiteral) == redacted,
          "HTML textContent omitted image, unsafe-link, code, or raw-HTML literals");

    check(markdown::toHtml("**safe** [docs](https://example.com)", ampersand) ==
              "<p><strong>safe</strong> <a href=\"https://example.com\">docs</a></p>",
          "safe Markdown lost rich HTML when no protected content was present");
    check(markdown::toHtml("[docs](https://example.com/safe%252Fpath)", ampersand) ==
              "<p><a href=\"https://example.com/safe%252Fpath\">docs</a></p>",
          "ordinary safe percent-encoded URL lost rich HTML");
    const auto generatedTag = [](std::string_view text) { return text.find("<strong>") != text.npos; };
    const auto safeLinkSuffix = [](std::string_view text) { return text.find("docs (https://example.com)") != text.npos; };
    check(markdown::toHtml("**safe**", generatedTag) == "<p><strong>safe</strong></p>" &&
              markdown::toHtml("[docs](https://example.com)", safeLinkSuffix) == "<p><a href=\"https://example.com\">docs</a></p>",
          "HTML markup or a safe-link destination was incorrectly included in textContent");
}

void nestedAndStable() {
    const std::string source = "## Title\n\n- one **bold _nested_**\n- [two](https://example.org)\n  - third `code`";
    const auto html = markdown::toHtml(source);
    const auto slackReply = markdown::toSlack(source);
    const auto& slack = slackReply.text;
    const auto plain = markdown::toPlain(source);
    check(html.find("<h2>Title</h2>") != std::string::npos && html.find("<strong>bold <em>nested</em></strong>") != std::string::npos &&
              html.find("<ul>") != std::string::npos && html.find("<code>code</code>") != std::string::npos,
          "Web rendering lost nested formatting or lists");
    check(slackReply.mrkdwn && slack.find("*bold _nested_*") != std::string::npos &&
              slack.find("<https://example.org|two>") != std::string::npos && slack.find("  • third `code`") != std::string::npos,
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
    check(slackMentions.mrkdwn &&
              slackMentions.text == "Notify &lt;!here&gt;, &lt;@U123&gt;, &lt;#C123&gt;, @channel, #general &amp; &lt;tag&gt;",
          "Slack rendered literal notification syntax as active markup");
    check(markdown::toPlain("~~literal~~") == "~~literal~~", "unsupported Markdown content was dropped");
}
}

void bareUrls() {
    check(markdown::toHtml("See https://example.com/pull/12, then (https://example.com/a_(b)) and https://example.com/x.") ==
              "<p>See <a href=\"https://example.com/pull/12\">https://example.com/pull/12</a>, then "
              "(<a href=\"https://example.com/a_(b)\">https://example.com/a_(b)</a>) and "
              "<a href=\"https://example.com/x\">https://example.com/x</a>.</p>",
          "bare URLs were not linked or kept trailing punctuation");
    check(markdown::toHtml("[docs](https://example.com/docs) and `https://example.com/code` and javascript:alert(1) and https://") ==
              "<p><a href=\"https://example.com/docs\">docs</a> and <code>https://example.com/code</code> and "
              "javascript:alert(1) and https://</p>",
          "autolinking touched existing links, code, unsafe schemes or empty URLs");
    check(markdown::toHtml("xhttps://example.com <https://example.com/&q> &amp;") ==
              "<p>xhttps://example.com <a href=\"https://example.com/&amp;q\">https://example.com/&amp;q</a> &amp;</p>",
          "autolinking matched inside a word or double-linked an explicit autolink");
    check(markdown::toHtml("See https://example.com/__init__.py and https://example.com/?q=*test*。") ==
              "<p>See <a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a> and "
              "<a href=\"https://example.com/?q=*test*\">https://example.com/?q=*test*</a>。</p>",
          "Markdown syntax or Japanese punctuation changed a bare link destination");
    check(markdown::toHtml("“https://example.com/資料” （https://example.com/report）") ==
              "<p>“<a href=\"https://example.com/資料\">https://example.com/資料</a>” "
              "（<a href=\"https://example.com/report\">https://example.com/report</a>）</p>",
          "Quotation marks or full-width parentheses entered a bare link");
    check(markdown::toHtml("> See **https://example.com/__init__.py**") ==
              "<blockquote><p>See <strong><a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a>"
              "</strong></p></blockquote>",
          "Linking a bare URL broke the enclosing Markdown");
    check(markdown::toHtml("https://example.com/[docs](https://example.org) and https://example.com/path**and more**") ==
              "<p><a href=\"https://example.com/\">https://example.com/</a><a href=\"https://example.org\">docs</a> and "
              "<a href=\"https://example.com/path\">https://example.com/path</a><strong>and more</strong></p>",
          "A bare link swallowed an existing Markdown link or incomplete emphasis");
    check(markdown::toHtml("[ref]: https://example.org/reference\nOpen https://example.com/?q=&amp; after") ==
              "<p>Open <a href=\"https://example.com/?q=&amp;\">https://example.com/?q=&amp;</a> after</p>",
          "Reference definitions or entity escaping changed a bare link");
    check(markdown::toHtml("**https://example.com/__init__.py**です") ==
              "<p><strong><a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a></strong>です</p>",
          "A bare URL changed its surrounding emphasis");
    check(markdown::toHtml("https://example.com/__init__.py[docs](https://example.org)") ==
              "<p><a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a>"
              "<a href=\"https://example.org\">docs</a></p>",
          "An adjacent Markdown link truncated the bare filename");
    check(markdown::toHtml("https://example.com/?q=*test*&amp;") ==
              "<p><a href=\"https://example.com/?q=*test*&amp;\">https://example.com/?q=*test*&amp;</a></p>",
          "A character reference at the end of a formatted URL changed its destination");
    check(markdown::toHtml("[ref]: https://example.org/reference\nOpen https://example.com/__init__.py after") ==
              "<p>Open <a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a> after</p>",
          "A preceding reference definition truncated the bare filename");
    check(markdown::toHtml("See the file:\n   https://example.com/__init__.py") ==
              "<p>See the file:<br>\n<a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a></p>",
          "Indentation truncated a bare filename on a continuation line");
    check(markdown::toHtml("> See the file:\nhttps://example.com/?a=1&amp;b=__init__.py") ==
              "<blockquote><p>See the file:<br>\n"
              "<a href=\"https://example.com/?a=1&amp;b=__init__.py\">https://example.com/?a=1&amp;b=__init__.py</a></p></blockquote>",
          "A lazy quoted continuation truncated a bare URL with a character reference");
    check(markdown::toHtml("https://example.com/**[docs](https://docs.example)**") ==
              "<p><a href=\"https://example.com/\">https://example.com/</a>"
              "<strong><a href=\"https://docs.example\">docs</a></strong></p>",
          "A bare link swallowed a link inside adjacent formatting");
    for (const auto prefix : {"> See\n ", "> See\n>  "})
        check(
            markdown::toHtml(std::string(prefix) + "https://example.com/__init__.py") ==
                "<blockquote><p>See<br>\n<a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a></p></blockquote>",
            "A space before a quoted continuation changed the bare link target");
    check(markdown::toHtml("> See\n>  https://example.com/__foo__ https://example.com/__bar__") ==
              "<blockquote><p>See<br>\n<a href=\"https://example.com/__foo__\">https://example.com/__foo__</a> "
              "<a href=\"https://example.com/__bar__\">https://example.com/__bar__</a></p></blockquote>",
          "Two similar URLs on a continuation line shared the wrong destination");
    check(markdown::toHtml("> See\n>  <https://example.com/__foo__> https://example.com/__bar__") ==
              "<blockquote><p>See<br>\n<a href=\"https://example.com/__foo__\">https://example.com/__foo__</a> "
              "<a href=\"https://example.com/__bar__\">https://example.com/__bar__</a></p></blockquote>",
          "A nearby explicit link replaced the bare link destination");
    check(markdown::toHtml(R"(https://example.com/__init__.py
More details\
Thanks)") == "<p><a href=\"https://example.com/__init__.py\">https://example.com/__init__.py</a><br>\n"
             "More details<br>\nThanks</p>",
          "A later backslash line break truncated an earlier bare URL");
    const auto containsCredential = [](std::string_view text) { return text.find("A&B") != text.npos; };
    check(markdown::toHtml("Open https://example.com/__init__.py?key=A%26B", containsCredential) == "<p>[redacted]</p>",
          "A bare URL exposed an encoded protected value");
}

int main() {
    try {
        issueExample();
        slackQueryUrl();
        slackCodeDelimiters();
        slackTextFallback();
        codeAndNewlines();
        definitionOnlySource();
        linksAndRawHtml();
        linkedImages();
        protectedHtml();
        nestedAndStable();
        bareUrls();
        std::cout << "Markdown rendering behavior passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
