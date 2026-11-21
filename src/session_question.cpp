#include "flock/core.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
std::string_view trimSessionQuestionText(std::string_view text) {
    static constexpr std::string_view whitespace[] = {
        " ", "\t", "\n", "\v", "\f", "\r", "\u00a0", "\u1680", "\u2000", "\u2001", "\u2002",
        "\u2003", "\u2004", "\u2005", "\u2006", "\u2007", "\u2008", "\u2009", "\u200a",
        "\u2028", "\u2029", "\u202f", "\u205f", "\u3000", "\ufeff"
    };
    while (!text.empty()) {
        const auto previousSize = text.size();
        for (auto space : whitespace) {
            if (text.starts_with(space)) text.remove_prefix(space.size());
            if (text.ends_with(space)) text.remove_suffix(space.size());
        }
        if (text.size() == previousSize) break;
    }
    return text;
}

namespace {
Json fieldAnswer(const Json& field, const Json& options, const Json& answer) {
    const auto selected = answer.is_array() ? answer : Json::array({answer});
    const bool multiple = field.value("multiple",false);
    if (selected.empty() || (!multiple && selected.size() != 1))
        throw Conflict("Question field answer has the wrong cardinality");

    std::set<std::string> seen;
    for (const auto& value : selected) {
        if (!value.is_string()) throw Conflict("Expected a textual question answer");
        const auto& text = value.get_ref<const std::string&>();
        if (text.size() > 8000 || !seen.insert(text).second)
            throw Conflict("Question answer is too long or repeated");
        if (std::ranges::any_of(options,[&](const Json& option) { return option.at("value") == value; }))
            continue;

        if (selected.size() != 1) throw Conflict("A freeform answer must be submitted on its own");
        const bool editor = field.contains("initialAnswer");
        const auto custom = editor ? std::string_view(text) : trimSessionQuestionText(text);
        if (!editor && custom.empty() && options.empty() && !field.value("required",true)) return "";
        if (!field.value("freeform",options.empty()) || (custom.empty() && !editor))
            throw Conflict("Answer does not match the question's input options");
        return std::string(custom);
    }
    return multiple ? selected : selected.front();
}
}

Json sessionQuestionAnswers(const Json& fields, const Json& answers) {
    if (!fields.is_array() || fields.empty() || !answers.is_object())
        throw Conflict("Answers are required per question field");

    auto result = Json::object();
    std::set<std::string> ids;
    for (const auto& field : fields) {
        const auto id = field.value("id",std::string{});
        if (id.empty() || !ids.insert(id).second) throw Conflict("Unsupported question fields");
        const auto options = field.value("options",Json::array());
        if (const auto answer = answers.find(id); answer != answers.end()) {
            result[id] = fieldAnswer(field,options,*answer);
        } else if (field.value("required",true)) {
            throw Conflict("A required question field was not answered");
        } else if (options.empty()) {
            result[id] = "";
        }
    }
    for (auto it = answers.begin(); it != answers.end(); ++it)
        if (!ids.contains(it.key())) throw Conflict("Answer references an unknown question field");
    return result;
}
}
