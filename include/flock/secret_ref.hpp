#pragma once

#include <string>

namespace flockv0 {
struct SecretRef {
    std::string name;
    bool valid() const;
};
}
