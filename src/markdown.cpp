#include "flock/markdown.hpp"

#include <cmark.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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

bool endsInCharacterReference(std::string_view url) {
    if (!url.ends_with(';')) return false;
    const auto ampersand = url.rfind('&');
    if (ampersand == url.npos || url.size() - ampersand < 3) return false;
    auto reference = url.substr(ampersand + 1, url.size() - ampersand - 2);
    if (reference.starts_with('#')) reference.remove_prefix(1);
    return !reference.empty() && std::ranges::all_of(reference, [](unsigned char ch) { return std::isalnum(ch); });
}

std::string_view bareUrlAt(std::string_view text) {
    if (!text.starts_with("http://") && !text.starts_with("https://")) return {};
    auto end =
        std::ranges::find_if(text, [](unsigned char ch) { return std::isspace(ch) || ch == '<' || ch == '>' || ch == '"' || ch == '`'; });
    auto url = text.substr(0, static_cast<std::size_t>(end - text.begin()));
    auto extraParentheses = std::ranges::count(url, ')') - std::ranges::count(url, '(');
    while (!url.empty() && !endsInCharacterReference(url)) {
        if (std::string_view(".,;:!?'").find(url.back()) != std::string_view::npos) {
            url.remove_suffix(1);
        } else if (url.back() == ')' && extraParentheses > 0) {
            url.remove_suffix(1);
            --extraParentheses;
        } else {
            static constexpr std::string_view closingPunctuation[] = {"。", "、", "，", "．", "！", "？", "）",
                                                                      "］", "」", "』", "”",  "’",  "】"};
            const auto suffix = std::ranges::find_if(closingPunctuation, [&](auto mark) { return url.ends_with(mark); });
            if (suffix == std::end(closingPunctuation)) break;
            url.remove_suffix(suffix->size());
        }
    }
    return safeUrl(url) ? url : std::string_view{};
}

std::vector<std::string_view> bareUrls(std::string_view text, std::size_t startColumn, std::size_t endColumn) {
    std::vector<std::string_view> urls;
    for (auto start = text.find("http", startColumn); start < endColumn; start = text.find("http", start + 1)) {
        if (!urls.empty() && text.data() + start < urls.back().data() + urls.back().size()) continue;
        if (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) || text[start - 1] == '/')) continue;
        if (const auto url = bareUrlAt(text.substr(start)); !url.empty()) urls.push_back(url);
    }
    return urls;
}

cmark_node* bareLink(const std::string& destination) {
    auto* link = cmark_node_new(CMARK_NODE_LINK);
    auto* label = cmark_node_new(CMARK_NODE_TEXT);
    cmark_node_set_url(link, destination.c_str());
    cmark_node_set_literal(label, destination.c_str());
    cmark_node_append_child(link, label);
    return link;
}

void promoteTextLinks(cmark_node* node) {
    const auto text = value(cmark_node_get_literal(node));
    const auto urls = bareUrls(text, 0, text.size());
    if (urls.empty()) return;
    std::size_t offset = 0;
    for (const auto url : urls) {
        const auto start = static_cast<std::size_t>(url.data() - text.data());
        if (start > offset) {
            auto* prefix = cmark_node_new(CMARK_NODE_TEXT);
            cmark_node_set_literal(prefix, text.substr(offset, start - offset).c_str());
            cmark_node_insert_before(node, prefix);
        }
        cmark_node_insert_before(node, bareLink(std::string(url)));
        offset = start + url.size();
    }
    cmark_node_set_literal(node, text.c_str() + offset);
}

std::vector<std::string_view> sourceLines(std::string_view source) {
    std::vector<std::string_view> lines;
    for (;;) {
        const auto end = source.find_first_of("\r\n");
        lines.push_back(source.substr(0, end));
        if (end == source.npos) return lines;
        source.remove_prefix(end + (source.substr(end).starts_with("\r\n") ? 2 : 1));
    }
}

std::size_t inlineContentEnd(cmark_node* node, int line, int adjustment, std::size_t lineLength) {
    for (auto* parent = cmark_node_parent(node); parent; parent = cmark_node_parent(parent)) {
        const auto type = cmark_node_get_type(parent);
        if (type != CMARK_NODE_EMPH && type != CMARK_NODE_STRONG) break;
        if (cmark_node_get_end_line(parent) != line) continue;
        const auto end = cmark_node_get_end_column(parent) + adjustment;
        const int delimiter = type == CMARK_NODE_STRONG ? 2 : 1;
        if (end >= delimiter) lineLength = std::min(lineLength, static_cast<std::size_t>(end - delimiter));
    }
    return lineLength;
}

bool isTextFormatting(cmark_node* node) {
    const auto type = cmark_node_get_type(node);
    if (type != CMARK_NODE_EMPH && type != CMARK_NODE_STRONG) return false;
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        if (cmark_node_get_type(child) != CMARK_NODE_TEXT && !isTextFormatting(child)) return false;
    return true;
}

cmark_node* containingParagraph(cmark_node* node) {
    for (auto* parent = cmark_node_parent(node); parent; parent = cmark_node_parent(parent))
        if (cmark_node_get_type(parent) == CMARK_NODE_PARAGRAPH) return parent;
    return nullptr;
}

int physicalSourceLine(cmark_node* node, cmark_node* paragraph) {
    const int line = cmark_node_get_start_line(node);
    if (!paragraph) return line;
    auto* last = cmark_node_last_child(paragraph);
    while (last && cmark_node_get_end_line(last) == 0) last = cmark_node_previous(last);
    return last ? line + std::max(0, cmark_node_get_end_line(paragraph) - cmark_node_get_end_line(last)) : line;
}

std::string decodeBareUrl(std::string_view url) {
    if (url.find('&') == url.npos) return std::string(url);
    const auto markdown = "<" + std::string(url) + ">";
    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root(cmark_parse_document(markdown.data(), markdown.size(), CMARK_OPT_DEFAULT),
                                                                 cmark_node_free);
    auto* paragraph = root ? cmark_node_first_child(root.get()) : nullptr;
    auto* link = paragraph ? cmark_node_first_child(paragraph) : nullptr;
    return link && cmark_node_get_type(link) == CMARK_NODE_LINK ? value(cmark_node_get_url(link)) : std::string(url);
}

std::string decodeUrlFragment(std::string_view fragment) {
    if (fragment.find('&') == fragment.npos) return std::string(fragment);
    constexpr std::string_view prefix = "https://url.invalid/";
    return decodeBareUrl(std::string(prefix) + std::string(fragment)).substr(prefix.size());
}

bool formattingMatchesSource(cmark_node* node, std::string_view source, int adjustment) {
    const int start = cmark_node_get_start_column(node) + adjustment;
    const int end = cmark_node_get_end_column(node) + adjustment;
    if (start < 1 || end < start || static_cast<std::size_t>(end) > source.size()) return false;
    const auto raw = source.substr(start - 1, end - start + 1);
    if (cmark_node_get_type(node) == CMARK_NODE_TEXT) return decodeUrlFragment(raw) == value(cmark_node_get_literal(node));
    if ((raw.front() != '*' && raw.front() != '_') || raw.back() != raw.front()) return false;
    for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child))
        if (!formattingMatchesSource(child, source, adjustment)) return false;
    return true;
}

struct FormattedBareUrl {
    std::string destination;
    std::string preceding;
    std::string following;
    cmark_node* last;
};

std::optional<FormattedBareUrl> formattedBareUrlAt(cmark_node* node, std::string_view raw, int adjustment) {
    const int line = cmark_node_get_start_line(node);
    const int column = cmark_node_get_start_column(node) + adjustment;
    const int end = cmark_node_get_end_column(node) + adjustment;
    if (column < 1 || end < column || cmark_node_get_end_line(node) != line) return std::nullopt;
    const auto content = raw.substr(0, inlineContentEnd(node, line, adjustment, raw.size()));
    const auto textEnd = std::min(content.size(), static_cast<std::size_t>(end));
    const auto literal = value(cmark_node_get_literal(node));

    for (auto url : bareUrls(content, column - 1, textEnd)) {
        const auto start = static_cast<std::size_t>(url.data() - content.data());
        if (start + url.size() <= textEnd) continue;
        const auto first = decodeBareUrl(url.substr(0, textEnd - start));
        if (!literal.ends_with(first)) continue;

        auto offset = textEnd;
        cmark_node* last = node;
        for (auto* next = cmark_node_next(node); next && offset < start + url.size(); next = cmark_node_next(next)) {
            const auto type = cmark_node_get_type(next);
            const bool formatting = isTextFormatting(next);
            const int nextEnd = cmark_node_get_end_column(next) + adjustment;
            if (cmark_node_get_start_line(next) != line || cmark_node_get_end_line(next) != line ||
                cmark_node_get_start_column(next) + adjustment != static_cast<int>(offset) + 1 || nextEnd <= static_cast<int>(offset) ||
                (type != CMARK_NODE_TEXT && !formatting) ||
                (formatting &&
                 (static_cast<std::size_t>(nextEnd) > start + url.size() || !formattingMatchesSource(next, content, adjustment))))
                break;
            offset = static_cast<std::size_t>(nextEnd);
            last = next;
        }
        if (last == node) continue;
        if (start + url.size() > offset) url = bareUrlAt(content.substr(start, offset - start));
        if (url.empty() || start + url.size() <= textEnd) continue;

        std::string following;
        if (cmark_node_get_type(last) == CMARK_NODE_TEXT) {
            const auto lastStart = static_cast<std::size_t>(cmark_node_get_start_column(last) + adjustment - 1);
            if (lastStart > start + url.size()) continue;
            const auto consumed = url.substr(lastStart - start);
            const auto decoded = decodeUrlFragment(consumed);
            const auto lastText = value(cmark_node_get_literal(last));
            if (!lastText.starts_with(decoded)) continue;
            following = lastText.substr(decoded.size());
        }
        const auto destination = decodeBareUrl(url);
        if (safeUrl(destination))
            return FormattedBareUrl{destination, literal.substr(0, literal.size() - first.size()), std::move(following), last};
    }
    return std::nullopt;
}

std::optional<FormattedBareUrl> formattedBareUrlOnLine(cmark_node* node, std::string_view raw) {
    if (auto span = formattedBareUrlAt(node, raw, 0)) return span;
    const auto literal = value(cmark_node_get_literal(node));
    std::optional<FormattedBareUrl> closest;
    auto distance = std::numeric_limits<int>::max();
    for (const auto url : bareUrls(raw, 0, raw.size())) {
        const auto sourceStart = static_cast<int>(url.data() - raw.data());
        for (auto marker = url.find_first_of("*_"); marker != url.npos; marker = url.find_first_of("*_", marker + 1)) {
            const auto prefix = url.substr(0, marker);
            const bool matches = prefix.find('&') == prefix.npos ? literal.ends_with(prefix) : literal.ends_with(decodeBareUrl(prefix));
            if (!matches) continue;
            const int adjustment = sourceStart + static_cast<int>(marker) - cmark_node_get_end_column(node);
            if (adjustment == 0 || std::abs(adjustment) >= distance) continue;
            if (auto span = formattedBareUrlAt(node, raw, adjustment)) {
                distance = std::abs(adjustment);
                closest = std::move(span);
            }
        }
    }
    return closest;
}

std::optional<FormattedBareUrl> formattedBareUrl(cmark_node* node, const std::vector<std::string_view>& lines) {
    auto* adjacent = cmark_node_next(node);
    if (!adjacent || !isTextFormatting(adjacent) || value(cmark_node_get_literal(node)).find("http") == std::string::npos)
        return std::nullopt;
    const int logicalLine = cmark_node_get_start_line(node);
    const int lastLine = physicalSourceLine(node, containingParagraph(node));
    for (int line = logicalLine; line <= lastLine && line > 0 && static_cast<std::size_t>(line) <= lines.size(); ++line)
        if (auto span = formattedBareUrlOnLine(node, lines[line - 1])) return span;
    return std::nullopt;
}

void promoteBareLinks(cmark_node* node, const std::vector<std::string_view>& lines) {
    const auto type = cmark_node_get_type(node);
    if (type == CMARK_NODE_LINK || type == CMARK_NODE_IMAGE) return;
    if (type != CMARK_NODE_TEXT) {
        for (auto* child = cmark_node_first_child(node); child; child = cmark_node_next(child)) promoteBareLinks(child, lines);
        return;
    }
    if (const auto span = formattedBareUrl(node, lines)) {
        auto* after = span->following.empty() ? cmark_node_next(span->last) : span->last;
        for (auto* consumed = cmark_node_next(node); consumed != after;) {
            auto* next = cmark_node_next(consumed);
            cmark_node_free(consumed);
            consumed = next;
        }
        auto* link = bareLink(span->destination);
        cmark_node_insert_after(node, link);
        cmark_node_set_literal(node, span->preceding.c_str());
        if (!span->following.empty()) cmark_node_set_literal(span->last, span->following.c_str());
    }
    promoteTextLinks(node);
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
    auto* document = cmark_parse_document(bytes, source.size(), target == Target::html ? CMARK_OPT_SOURCEPOS : CMARK_OPT_DEFAULT);
    if (!document) return escape(source, target);
    std::unique_ptr<cmark_node, decltype(&cmark_node_free)> root(document, cmark_node_free);
    if (target == Target::html && source.find("http") != source.npos) promoteBareLinks(root.get(), sourceLines(source));
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
