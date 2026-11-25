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

class Capture {
public:
    static Capture* configured(Provider) noexcept;

    Capture(std::filesystem::path, Provider, std::string commit);
    ~Capture();

    bool append(std::string_view revision, const std::function<Json(Ids&)>& projection) noexcept;
private:
    std::filesystem::path path_;
    Provider provider_;
    std::string commit_;
    std::mutex mutex_;
    Ids ids_;
    int file_ = -1;
    bool writeFailed_ = false;

    bool write(std::string_view line);
};

}
