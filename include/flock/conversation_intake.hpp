#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <nlohmann/json.hpp>
#include <string_view>

namespace flockv0 {
class Store;
class ConversationTurns;
struct Intake;
struct Person;

class ConversationIntake {
public:
    explicit ConversationIntake(Store&);

    // Enqueues a Bot message for Runtime. Retrying the same key and payload returns the same receipt.
    Json receive(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text, Id replyToMessageId = 0,
                 const Json& attachments = Json::array());
private:
    ConversationTurns& turns_;
};
}
