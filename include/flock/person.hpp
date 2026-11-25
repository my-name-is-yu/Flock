#pragma once

#include <compare>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace flockv0 {
struct Person {
    std::string id;
    bool adminRoot = false;
    auto operator<=>(const Person&) const = default;
};
inline const Person localOwner{"local-owner", true};

// An absent result means the person is unavailable. Otherwise the value is their current root authority.
using WebIdentityResolver = std::function<std::optional<bool>(std::string_view personId)>;
}
