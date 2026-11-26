#pragma once

#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <span>
#include <string>
#include <string_view>

namespace flockv0 {
class Store;
struct Person;

struct SuggestedTaskStart {
    Id id;
    std::string request;
};

inline constexpr unsigned maxSuggestedTasks = 5;

class SuggestedTasks {
public:
    explicit SuggestedTasks(Store& store) : store_(store) {}

    Json start(const Person&, std::string_view bot, Id sourceMessageId, std::span<const SuggestedTaskStart>, const IntakeResolver&);
    void dismiss(const Person&, std::string_view bot, Id id);
private:
    Store& store_;
};
}
