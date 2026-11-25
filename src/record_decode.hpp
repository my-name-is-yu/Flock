#pragma once

#include "flock/core.hpp"
#include "flock/record_decode_error.hpp"

#include <algorithm>
#include <array>
#include <type_traits>

namespace flockv0 {
template <class Enum, std::size_t N> std::string_view enumName(const std::array<std::pair<Enum, std::string_view>, N>& names, Enum value) {
    const auto match = std::ranges::find(names, value, &std::pair<Enum, std::string_view>::first);
    if (match == names.end()) throw std::logic_error("Invalid record enum value");
    return match->second;
}

template <class Enum, std::size_t N>
std::optional<Enum> enumValue(const std::array<std::pair<Enum, std::string_view>, N>& names, std::string_view text) {
    const auto match = std::ranges::find(names, text, &std::pair<Enum, std::string_view>::second);
    if (match == names.end()) return std::nullopt;
    return match->first;
}

class RecordDecoder {
public:
    RecordDecoder(std::string_view table, std::string_view id, std::string path = {}) : table_(table), id_(id), path_(std::move(path)) {}

    RecordDecoder nested(std::string_view key) const { return {table_, id_, qualify(key)}; }

    [[noreturn]] void fail(std::string_view key, std::string_view issue) const {
        throw RecordDecodeError(table_, id_, qualify(key), issue);
    }

    Json parse(std::string_view text) const {
        auto data = Json::parse(text, nullptr, false);
        if (data.is_discarded()) fail("data", "invalid JSON");
        return data;
    }

    const Json& require(const Json& object, std::string_view key) const {
        if (!object.is_object()) fail({}, "expected an object");
        const auto found = object.find(key);
        if (found == object.end()) fail(key, "missing required key");
        return *found;
    }

    const Json& object(const Json& value, std::string_view key = {}) const {
        if (!value.is_object()) fail(key, "expected an object");
        return value;
    }

    const Json& array(const Json& value, std::string_view key = {}) const {
        if (!value.is_array()) fail(key, "expected an array");
        return value;
    }

    template <class T> T value(const Json& input, std::string_view key = {}) const {
        if constexpr (std::is_same_v<T, std::string>) {
            if (!input.is_string()) fail(key, "expected a string");
        } else if constexpr (std::is_same_v<T, bool>) {
            if (!input.is_boolean()) fail(key, "expected a boolean");
        } else if constexpr (std::is_integral_v<T>) {
            if (!input.is_number_integer()) fail(key, "expected an integer");
            if (input.is_number_unsigned()) {
                if (!std::in_range<T>(input.get<Json::number_unsigned_t>())) fail(key, "integer out of range");
            } else if (!std::in_range<T>(input.get<Json::number_integer_t>()))
                fail(key, "integer out of range");
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            array(input, key);
            T values;
            for (const auto& item : input) values.push_back(value<std::string>(item, key));
            return values;
        }
        return input.get<T>();
    }

    template <class T> T get(const Json& object, std::string_view key) const { return value<T>(require(object, key), key); }

    template <class T> std::optional<T> optional(const Json& object, std::string_view key) const {
        if (!object.contains(key)) return std::nullopt;
        return get<T>(object, key);
    }

    template <class T> std::optional<T> nullable(const Json& object, std::string_view key) const {
        const auto& input = require(object, key);
        if (input.is_null()) return std::nullopt;
        return value<T>(input, key);
    }
private:
    std::string_view table_, id_;
    std::string path_;

    std::string qualify(std::string_view key) const {
        if (path_.empty()) return key.empty() ? "data" : std::string(key);
        return key.empty() ? path_ : path_ + "." + std::string(key);
    }
};

inline RunRef decodeRunRef(const Json& data, const RecordDecoder& row) {
    return {row.get<std::string>(data, "threadId"), row.get<std::string>(data, "runId"), row.get<std::string>(data, "link"),
            row.optional<std::string>(data, "gatewayEnvironmentId").value_or("")};
}

inline std::optional<RunAssignment> decodeRunAssignment(const Json& data, const RecordDecoder& row) {
    if (!data.contains("ownerBotId") && !data.contains("projectId") && !data.contains("revisionId")) return std::nullopt;
    return RunAssignment{row.get<std::string>(data, "ownerBotId"), row.get<std::string>(data, "projectId"),
                         row.nullable<Id>(data, "revisionId")};
}

inline void encodeRunAssignment(Json& data, const std::optional<RunAssignment>& assignment) {
    if (!assignment) return;
    data["ownerBotId"] = assignment->ownerBotId;
    data["projectId"] = assignment->projectId;
    data["revisionId"] = assignment->revisionId ? Json(*assignment->revisionId) : Json(nullptr);
}
}
