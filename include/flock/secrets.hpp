#pragma once

#include "flock/core.hpp"

namespace flockv0 {
class SecretRedactor {
public:
    explicit SecretRedactor(std::vector<std::string> values);
    bool empty() const { return values_.empty(); }
    std::string text(std::string_view source, bool truncated = false) const;
    Json payload(const Json&, bool truncated = false, std::size_t textLimit = std::string_view::npos) const;
    enum class Fields { protocol, external };
    Json record(Json, Fields = Fields::protocol) const;
    void requirePublic(const Json&) const;
    bool containsProtectedText(std::string_view) const;
    bool containsProtected(const Json&) const;
private:
    std::vector<std::string> values_;
};
}
