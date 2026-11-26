#pragma once

#include "flock/types.hpp"

#include <functional>
#include <string_view>

namespace flockv0 {
class Store;
struct Person;
struct SecretRef;

class SecretRequests {
public:
    explicit SecretRequests(Store& store) : store_(store) {}

    Id requestSecret(std::string_view bot, const SecretRef&, Id sourceMessage = 0);
    void submitSecret(const Person&, std::string_view bot, Id request, std::string_view value);
    void useSecret(Id task, const SecretRef&, std::string_view destination, std::string_view operation, bool upstreamPermitted,
                   const std::function<void(std::string_view)>& adapter) const;
private:
    Store& store_;
};
}
