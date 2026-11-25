#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace flockv0 {
using Id = std::int64_t;
using Seconds = std::int64_t;

Seconds unixTime();
std::string bounded(std::string_view text, std::size_t bytes);
std::string actionId();

struct Conflict : std::runtime_error {
    using std::runtime_error::runtime_error;
};
}
