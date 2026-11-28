#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

namespace flockv0 {
class Store;
class ConversationTurns;
class Database;
class Policy;
struct Intake;
struct Person;

class WebConversationIntake {
public:
    explicit WebConversationIntake(Store&);

    // Refers to a person in the current Web identity directory when the Runtime executes the turn.
    Json receiveWeb(const Person&, const Intake& routing, std::string_view clientMessageId, std::string_view text, Id replyToMessageId = 0,
                    const Json& attachments = Json::array());
    std::optional<Json> receipt(const Person&, std::string_view botId, std::string_view clientMessageId, std::string_view text,
                                Id replyToMessageId = 0, const Json& attachments = Json::array()) const;
private:
    Database& database_;
    ConversationTurns& turns_;
    const Policy& permissions_;
};
}
