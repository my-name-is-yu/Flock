#pragma once

#include "flock/core.hpp"

#include <initializer_list>
#include <map>
#include <mutex>

namespace flockv0::recording {

enum class Provider { T3, ChatGPT };

Json redactedShape(const Json& value);
Json literalOrRedacted(const Json& value, std::initializer_list<std::string_view> allowed);

class Ids {
public:
    Json replace(std::string_view domain, const Json& value);
private:
    std::map<std::string, std::map<std::string, std::size_t>> domains_;
};

class Fields {
public:
    explicit Fields(const Json& source) : source_(source), result_(redactedShape(source)) {}

    void pick(std::string_view key, const auto& project) {
        if (const auto value = source_.find(key); value != source_.end()) result_[key] = project(*value);
    }

    void redact(std::initializer_list<std::string_view> keys);
    void counters(std::initializer_list<std::string_view> keys);
    void ids(Ids&, std::string_view domain, std::initializer_list<std::string_view> keys);
    void literal(std::string_view key, std::initializer_list<std::string_view> allowed);
    Json take() { return std::move(result_); }
private:
    const Json& source_;
    Json result_;
};

Json projectArray(const Json& value, const auto& project) {
    if (!value.is_array()) return redactedShape(value);
    auto result = Json::array();
    for (const auto& item : value) result.push_back(project(item));
    return result;
}

class Capture {
public:
    static Capture* configured(Provider) noexcept;

    Capture(std::filesystem::path, Provider, std::string commit, std::string selectedRevision = {});
    ~Capture();

    bool append(std::string_view revision, const std::function<Json(Ids&)>& projection) noexcept;
private:
    std::filesystem::path path_;
    Provider provider_;
    std::string commit_;
    std::string selectedRevision_;
    std::mutex mutex_;
    Ids ids_;
    int file_ = -1;
    bool writeFailed_ = false;

    bool write(std::string_view line);
};

}
