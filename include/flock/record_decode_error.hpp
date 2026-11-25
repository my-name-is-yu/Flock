#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace flockv0 {
class RecordDecodeError : public std::runtime_error {
public:
    RecordDecodeError(std::string_view table, std::string_view row, std::string_view key, std::string_view issue)
        : std::runtime_error(std::string(table) + " row " + std::string(row) + ", key " + std::string(key) + ": " + std::string(issue)) {}
};
}
