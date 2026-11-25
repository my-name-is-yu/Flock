#include "flock/markdown.hpp"

#include <cmark.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace flockv0::markdown {
namespace {
enum class Target { html, html_text, slack, slack_plain, plain };

std::string value(const char* source) {
    return source ? source : "";
}

std::string escape(std::string_view source, Target target, bool attribute = false) {
    if (target == Target::plain || target == Target::html_text) return std::string(source);
    std::string result;
    for (const char ch : source) {
        if (ch == '&')
            result += "&amp;";
        else if (ch == '<')
            result += "&lt;";
        else if (ch == '>')
            result += "&gt;";
        else if (attribute && ch == '"')
            result += "&quot;";
        else if (attribute && ch == '\'')
            result += "&#39;";
        else
            result += ch;
    }
    return result;
}

bool hasNonWhitespace(std::string_view source) {
    return std::ranges::any_of(source, [](unsigned char ch) { return !std::isspace(ch); });
}

std::optional<std::string> urlPart(CURLU* url, CURLUPart part) {
    char* result = nullptr;
    if (curl_url_get(url, part, &result, 0) != CURLUE_OK) return std::nullopt;
    std::string copy(result);
    curl_free(result);
    return copy;
}

bool curlReady() {
    static std::once_flag initialized;
    static bool ready = false;
    std::call_once(initialized, [] { ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; });
    return ready;
}

std::optional<std::string> percentDecode(std::string_view source) {
    if (source.empty()) return std::string{};
    if (source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) || !curlReady()) return std::nullopt;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return std::nullopt;
    int length = 0;
    auto* decoded = curl_easy_unescape(curl.get(), source.data(), static_cast<int>(source.size()), &length);
    if (!decoded) return std::nullopt;
    std::string result(decoded, static_cast<std::size_t>(length));
    curl_free(decoded);
    return result;
}

std::optional<std::string> formPercentDecode(std::string_view source) {
    std::string form(source);
    for (char& character : form)
        if (character == '+') character = ' ';
    return percentDecode(form);
}

template <typename Decode> bool requiresUrlRedactionAfterDecoding(std::string_view source, const SensitiveText& sensitive, Decode decode) {
    constexpr unsigned maxPercentDecodeLayers = 4;
    std::string current(source);
    for (unsigned layer = 0; layer <= maxPercentDecodeLayers; ++layer) {
        if (sensitive && sensitive(current)) return true;
        const auto decoded = decode(current);
        if (!decoded) return true;
        if (*decoded == current) return false;
        if (layer == maxPercentDecodeLayers) return true;
        current = *decoded;
    }
    return true;
}

bool sensitiveUrl(std::string_view source, const SensitiveText& sensitive) {
    if (source.empty()) return false;
    if (requiresUrlRedactionAfterDecoding(source, sensitive, percentDecode)) return true;
    const auto query = source.find('?');
    const auto fragment = source.find('#');
    if (query == source.npos || (fragment != source.npos && fragment < query)) return false;
    const auto end = fragment == source.npos ? source.size() : fragment;
    return requiresUrlRedactionAfterDecoding(source.substr(query + 1, end - query - 1), sensitive, formPercentDecode);
}

bool safeUrl(std::string_view source) {
    if (source.empty() || source.find_first_of("<>|\\") != source.npos) return false;
    for (const unsigned char ch : source)
        if (ch <= 32 || ch == 127) return false;
    const auto colon = source.find(':');
    if (colon == source.npos) return false;
    std::string rawScheme(source.substr(0, colon));
    std::ranges::transform(rawScheme, rawScheme.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if ((rawScheme != "http" && rawScheme != "https") || source.substr(colon, 3) != "://") return false;
    if (!curlReady()) return false;
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, std::string(source).c_str(), 0) != CURLUE_OK) return false;
    const auto scheme = urlPart(parsed.get(), CURLUPART_SCHEME);
    const auto host = urlPart(parsed.get(), CURLUPART_HOST);
    if (!scheme || !host || host->empty() || host->find('%') != host->npos) return false;
    std::string normalizedScheme = *scheme;
    std::ranges::transform(normalizedScheme, normalizedScheme.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (normalizedScheme != "http" && normalizedScheme != "https") return false;
    char* user = nullptr;
    const bool hasUser = curl_url_get(parsed.get(), CURLUPART_USER, &user, 0) == CURLUE_OK;
    curl_free(user);
    char* password = nullptr;
    const bool hasPassword = curl_url_get(parsed.get(), CURLUPART_PASSWORD, &password, 0) == CURLUE_OK;
    curl_free(password);
    return !hasUser && !hasPassword;
}

bool block(cmark_node_type type) {
    return type >= CMARK_NODE_FIRST_BLOCK && type <= CMARK_NODE_LAST_BLOCK;
}

std::string children(cmark_node* node, Target target, unsigned depth);
std::string render(cmark_node* node, Target target, unsigned depth);

std::string inlineChildren(cmark_node* node, Target target, unsigned depth) {
    std::string result;
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child)) result += render(child, target, depth);
    return result;
}

std::string children(cmark_node* node, Target target, unsigned depth) {
    std::string result;
    bool previousBlock = false;
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child)) {
        const bool currentBlock = block(cmark_node_get_type(child));
        if (previousBlock && currentBlock) result += (target == Target::html || target == Target::html_text) ? "\n" : "\n\n";
        result += render(child, target, depth);
        previousBlock = currentBlock;
    }
    return result;
}

std::string quoteLines(std::string source) {
    if (source.empty()) return "> ";
    std::string result = "> ";
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char ch = source[index];
        result += ch;
        if (ch == '\n' && index + 1 < source.size()) result += "> ";
    }
    return result;
}

std::string listItems(cmark_node* node, Target target, unsigned depth) {
    const bool ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
    auto number = cmark_node_get_list_start(node);
    const auto indent = std::string(depth * 2, ' ');
    std::string result;
    bool firstItem = true;
    for (auto* item = cmark_node_first_child(node); item; item = cmark_node_next(item)) {
        if (!firstItem) result += "\n";
        firstItem = false;
        const auto marker = ordered ? std::to_string(number++) + ". " : "• ";
        bool firstBlock = true;
        for (auto* child = cmark_node_first_child(item); child; child = cmark_node_next(child)) {
            if (cmark_node_get_type(child) == CMARK_NODE_PARAGRAPH) {
                if (!firstBlock)
                    result += "\n" + indent + std::string(marker.size(), ' ');
                else
                    result += indent + marker;
                result += inlineChildren(child, target, depth);
            } else if (cmark_node_get_type(child) == CMARK_NODE_LIST) {
                if (!firstBlock) result += "\n";
                result += listItems(child, target, depth + 1);
            } else {
                if (!firstBlock)
                    result += "\n" + indent + std::string(marker.size(), ' ');
                else
                    result += indent + marker;
                result += render(child, target, depth + 1);
            }
            firstBlock = false;
        }
    }
    return result;
}

std::string renderList(cmark_node* node, Target target, unsigned depth) {
    if (target == Target::html_text) {
        std::string result;
        for (auto* item = cmark_node_first_child(node); item; item = cmark_node_next(item)) result += children(item, target, depth + 1);
        return result;
    }
    if (target != Target::html) return listItems(node, target, depth);
    const bool ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
    const int start = cmark_node_get_list_start(node);
    std::string result = ordered ? "<ol" : "<ul";
    if (ordered && start != 1) result += " start=\"" + std::to_string(start) + "\"";
    result += ">";
    for (auto* item = cmark_node_first_child(node); item; item = cmark_node_next(item))
        result += "<li>" + children(item, target, depth + 1) + "</li>";
    result += ordered ? "</ol>" : "</ul>";
    return result;
}

std::string fallbackLink(cmark_node* node, Target target, unsigned depth) {
    const auto label = inlineChildren(node, target, depth);
    const auto destination = value(cmark_node_get_url(node));
    if (destination.empty()) return label;
    const auto safe = safeUrl(destination);
    if (target == Target::html) {
        const auto href = safe ? "<a href=\"" + escape(destination, target, true) + "\">" + escape(destination, target) + "</a>"
                               : escape(destination, target);
        return label + " (" + href + ")";
    }
    if (target == Target::slack || target == Target::slack_plain) {
        const auto link = target == Target::slack && safe ? "<" + escape(destination, target) + ">" : escape(destination, target);
        return label + " (" + link + ")";
    }
    return label + "（" + destination + "）";
}

std::string link(cmark_node* node, Target target, unsigned depth) {
    const auto destination = value(cmark_node_get_url(node));
    const auto label = inlineChildren(node, target, depth);
    const auto safe = safeUrl(destination);
    if (target == Target::html_text) {
        if (destination.empty() || safe) return label;
        return label + " (" + destination + ")";
    }
    if (!safe) return fallbackLink(node, target, depth);
    if (target == Target::html) return "<a href=\"" + escape(destination, target, true) + "\">" + label + "</a>";
    if (target == Target::slack) {
        if (label.find('|') != label.npos || label.find('\n') != label.npos) return fallbackLink(node, target, depth);
        return "<" + escape(destination, target) + "|" + label + ">";
    }
    if (target == Target::slack_plain) return label + " (" + escape(destination, target) + ")";
    return label + "（" + destination + "）";
}

bool insideLink(cmark_node* node) {
    for (auto* parent = cmark_node_parent(node); parent; parent = cmark_node_parent(parent))
        if (cmark_node_get_type(parent) == CMARK_NODE_LINK) return true;
    return false;
}

std::string image(cmark_node* node, Target target, unsigned depth) {
    const auto alt = inlineChildren(node, target, depth);
    const auto destination = value(cmark_node_get_url(node));
    if (destination.empty()) return alt;
    if (target == Target::html_text) return alt + " (" + destination + ")";
    const auto safe = safeUrl(destination);
    const auto linked = insideLink(node);
    if (target == Target::html) {
        const auto url = safe && !linked ? "<a href=\"" + escape(destination, target, true) + "\">" + escape(destination, target) + "</a>"
                                         : escape(destination, target);
        return alt + " (" + url + ")";
    }
    if (target == Target::slack || target == Target::slack_plain) {
        const auto url = target == Target::slack && safe && !linked ? "<" + escape(destination, target) + ">" : escape(destination, target);
        return alt + " (" + url + ")";
    }
    return alt + "（" + destination + "）";
}

std::string code(std::string_view content, Target target, bool fenced) {
    if (target == Target::html_text) return std::string(content);
    if (target == Target::html) {
        if (fenced) return "<pre><code>" + escape(content, target) + "</code></pre>";
        return "<code>" + escape(content, target) + "</code>";
    }
    if (target == Target::plain) return std::string(content);
    if (target == Target::slack_plain) return escape(content, target);
    if (fenced) {
        std::string result = "```\n" + escape(content, target);
        if (!content.ends_with('\n')) result += '\n';
        return result + "```";
    }
    return "`" + escape(content, target) + "`";
}

std::string render(cmark_node* node, Target target, unsigned depth) {
    const auto type = cmark_node_get_type(node);
    switch (type) {
    case CMARK_NODE_DOCUMENT: return children(node, target, depth);
    case CMARK_NODE_PARAGRAPH:
        if (target == Target::html) return "<p>" + inlineChildren(node, target, depth) + "</p>";
        return inlineChildren(node, target, depth);
    case CMARK_NODE_HEADING: {
        const auto content = inlineChildren(node, target, depth);
        if (target == Target::html) {
            const auto level = std::clamp(cmark_node_get_heading_level(node), 1, 6);
            const auto tag = std::to_string(level);
            return "<h" + tag + ">" + content + "</h" + tag + ">";
        }
        return target == Target::slack ? "*" + content + "*" : content;
    }
    case CMARK_NODE_BLOCK_QUOTE: {
        const auto content = children(node, target, depth + 1);
        if (target == Target::html) return "<blockquote>" + content + "</blockquote>";
        if (target == Target::html_text) return content;
        return quoteLines(content);
    }
    case CMARK_NODE_LIST: return renderList(node, target, depth);
    case CMARK_NODE_ITEM: return children(node, target, depth);
    case CMARK_NODE_CODE_BLOCK: return code(value(cmark_node_get_literal(node)), target, true);
    case CMARK_NODE_HTML_BLOCK:
    case CMARK_NODE_HTML_INLINE: return escape(value(cmark_node_get_literal(node)), target);
    case CMARK_NODE_THEMATIC_BREAK: return target == Target::html ? "<hr>" : target == Target::html_text ? "" : "---";
    case CMARK_NODE_TEXT: return escape(value(cmark_node_get_literal(node)), target);
    case CMARK_NODE_SOFTBREAK:
    case CMARK_NODE_LINEBREAK: return target == Target::html ? "<br>\n" : "\n";
    case CMARK_NODE_CODE: return code(value(cmark_node_get_literal(node)), target, false);
    case CMARK_NODE_EMPH: {
        const auto content = inlineChildren(node, target, depth);
        if (target == Target::html) return "<em>" + content + "</em>";
        return target == Target::slack ? "_" + content + "_" : content;
    }
    case CMARK_NODE_STRONG: {
        const auto content = inlineChildren(node, target, depth);
        if (target == Target::html) return "<strong>" + content + "</strong>";
        return target == Target::slack ? "*" + content + "*" : content;
    }
    case CMARK_NODE_LINK: return link(node, target, depth);
    case CMARK_NODE_IMAGE: return image(node, target, depth);
    case CMARK_NODE_CUSTOM_BLOCK:
    case CMARK_NODE_CUSTOM_INLINE: {
        auto result = escape(value(cmark_node_get_on_enter(node)), target);
        result += children(node, target, depth);
        result += escape(value(cmark_node_get_on_exit(node)), target);
        return result;
    }
    default: return children(node, target, depth);
    }
}

bool hasSensitiveUrl(cmark_node* node, const SensitiveText& sensitive) {
    const auto type = cmark_node_get_type(node);
    if ((type == CMARK_NODE_LINK || type == CMARK_NODE_IMAGE) && sensitiveUrl(value(cmark_node_get_url(node)), sensitive)) return true;
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        if (hasSensitiveUrl(child, sensitive)) return true;
    return false;
}

bool slackMrkdwnRepresentable(cmark_node* node) {
    const auto type = cmark_node_get_type(node);
    const auto literal = value(cmark_node_get_literal(node));
    if (type == CMARK_NODE_CODE) return literal.find('`') == std::string::npos;
    if (type == CMARK_NODE_CODE_BLOCK) return literal.find("```") == std::string::npos;
    if ((type == CMARK_NODE_TEXT || type == CMARK_NODE_HTML_INLINE || type == CMARK_NODE_HTML_BLOCK) &&
        literal.find_first_of("*_~`") != std::string::npos)
        return false;
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        if (!slackMrkdwnRepresentable(child)) return false;
    return true;
}

std::string convert(std::string_view source, Target target, const SensitiveText& sensitive = {}) {
    static constexpr char empty[] = "";
    const auto* bytes = source.empty() ? empty : source.data();
    auto* document = cmark_parse_document(bytes, source.size(), CMARK_OPT_DEFAULT);
    if (!document) return escape(source, target);
    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root(document, cmark_node_free);
    if (target == Target::html &&
        ((sensitive && sensitive(render(root.get(), Target::html_text, 0))) || hasSensitiveUrl(root.get(), sensitive)))
        return "<p>[redacted]</p>";
    const auto rendered = render(root.get(), target, 0);
    if (rendered.empty() && hasNonWhitespace(source)) return escape(source, target);
    return rendered;
}
}

std::string toHtml(std::string_view source) {
    return convert(source, Target::html);
}
std::string toHtml(std::string_view source, const SensitiveText& sensitive) {
    return convert(source, Target::html, sensitive);
}
SlackReply toSlack(std::string_view source) {
    static constexpr char empty[] = "";
    const auto* bytes = source.empty() ? empty : source.data();
    auto* document = cmark_parse_document(bytes, source.size(), CMARK_OPT_DEFAULT);
    if (!document) return {escape(source, Target::slack_plain), false};
    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root(document, cmark_node_free);
    const bool mrkdwn = slackMrkdwnRepresentable(root.get());
    const auto target = mrkdwn ? Target::slack : Target::slack_plain;
    const auto rendered = render(root.get(), target, 0);
    if (rendered.empty() && hasNonWhitespace(source)) return {escape(source, Target::slack_plain), false};
    return {rendered, mrkdwn};
}
std::string toPlain(std::string_view source) {
    return convert(source, Target::plain);
}
}
