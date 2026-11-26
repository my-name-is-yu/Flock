#include "memory_learnings.hpp"
#include "flock/limits.hpp"
#include "flock/secrets.hpp"

#include <algorithm>
#include <ranges>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

namespace flockv0 {
namespace {
constexpr std::string_view heading = "## Learned from work";
constexpr std::string_view bullet = "- ";

std::string normalized(std::string_view text) {
    std::string key;
    bool gap = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\r') {
            gap = !key.empty();
            continue;
        }
        if (gap) key += ' ';
        gap = false;
        key += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return key;
}

bool safeLearningText(std::string_view text, const SecretRedactor& scrub) {
    return text.size() <= limits::memory::maxLearningBytes && text.find_first_not_of(" \t") != text.npos &&
           std::ranges::none_of(text, [](unsigned char c) { return (c < ' ' && c != '\t') || c == 127; }) &&
           text.find("[redacted]") == text.npos && scrub.text(text) == text;
}

std::string_view nextLine(std::string_view& text) {
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text.remove_prefix(end == text.npos ? text.size() : end + 1);
    return line;
}

struct LearnedSection {
    std::string_view before, after;
    std::vector<std::string_view> facts;
    bool exists, trailingNewline;
};

LearnedSection learnedSection(std::string_view content) {
    auto rest = content;
    while (!rest.empty()) {
        const auto offset = content.size() - rest.size();
        if (nextLine(rest) != heading) continue;
        LearnedSection section{content.substr(0, offset), {}, {}, true, content.ends_with('\n')};
        while (rest.starts_with(bullet)) section.facts.push_back(nextLine(rest).substr(bullet.size()));
        section.after = rest;
        return section;
    }
    return {{}, content, {}, false, true};
}

std::string writeSection(const LearnedSection& section, const std::vector<std::string>& added, const std::vector<std::string>& removed) {
    std::string result(section.before);
    result.append(heading).append("\n");
    for (const auto& fact : added) result.append(bullet).append(fact).append("\n");
    for (const auto fact : section.facts)
        if (std::ranges::find(removed, fact) == removed.end()) result.append(bullet).append(fact).append("\n");
    if (!section.exists && !section.after.empty() && !section.after.starts_with('\n')) result += '\n';
    if (section.after.empty() && !section.trailingNewline) result.pop_back();
    result.append(section.after);
    return result;
}
}

Json safeTaskLearnings(const Json& proposed, const SecretRedactor& scrub) {
    auto safe = Json::array();
    if (!proposed.is_array() || proposed.size() > limits::memory::maxLearnings) return safe;
    for (const auto& item : proposed) {
        if (!item.is_object() || item.size() != (item.contains("replaces") ? 3 : 2)) continue;
        const auto text = field(item, "text");
        const auto run = field(item, "runId");
        if (run.empty() || scrub.containsProtected(item) || !safeLearningText(text, scrub)) continue;
        if (item.contains("replaces") && !safeLearningText(field(item, "replaces"), scrub)) continue;
        safe.push_back(item);
    }
    return safe;
}

LearnedMemory mergeTaskLearnings(std::string_view memory, const Json& proposed, std::string_view runId, const SecretRedactor& scrub) {
    LearnedMemory result{std::string(memory), {}, {}};
    auto learnings = safeTaskLearnings(proposed, scrub);
    std::stable_partition(learnings.begin(), learnings.end(), [](const Json& fact) { return fact.contains("replaces"); });
    const auto section = learnedSection(memory);
    std::multiset<std::string> known;
    auto rest = memory;
    while (!rest.empty()) {
        auto line = nextLine(rest);
        if (line.starts_with(bullet)) line.remove_prefix(bullet.size());
        known.insert(normalized(line));
    }

    for (const auto& item : learnings) {
        if (item.at("runId") != runId) continue;
        const auto& text = item.at("text").get_ref<const std::string&>();
        const auto replaces = field(item, "replaces");
        if (!replaces.empty() && (text == replaces || std::ranges::count(section.facts, replaces) != 1 ||
                                  std::ranges::find(result.removed, replaces) != result.removed.end()))
            continue;
        auto key = normalized(text);
        const auto replaced = replaces.empty() ? known.end() : known.find(normalized(replaces));
        const auto allowedMatches = replaced != known.end() && *replaced == key ? 1u : 0u;
        if (known.count(key) > allowedMatches) continue;
        result.added.push_back(text);
        if (!replaces.empty()) result.removed.push_back(replaces);
        auto next = writeSection(section, result.added, result.removed);
        if (next.size() > limits::memory::maxDocumentBytes) {
            result.added.pop_back();
            if (!replaces.empty()) result.removed.pop_back();
            continue;
        }
        result.content = std::move(next);
        if (replaced != known.end()) known.erase(replaced);
        known.insert(std::move(key));
    }
    return result;
}
}
