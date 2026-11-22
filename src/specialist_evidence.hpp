#pragma once

#include "conversation_storage.hpp"

namespace flockv0 {

inline std::optional<std::string> specialistFindings(const Json& report) {
    const auto value = report.find("findings");
    if (value == report.end()) return {};
    if (value->is_string()) {
        const auto& text = value->get_ref<const std::string&>();
        return conversation_storage::hasText(text) ? std::optional(text) : std::nullopt;
    }
    if (!value->is_array() || value->empty() || value->size() > 8) return {};
    std::string result;
    for (const auto& item : *value) {
        if (!item.is_string()) return {};
        const auto& text = item.get_ref<const std::string&>();
        if (!conversation_storage::hasText(text)) return {};
        if (!result.empty()) result += '\n';
        result += text;
    }
    return result;
}

}
