#pragma once
#include "flock/json.hpp"
#include <nlohmann/json.hpp>
#include "flock/types.hpp"

#include <optional>
#include <string_view>

namespace flockv0 {
enum class InboxKind { user_question, protected_input, policy_approval };

std::string_view inboxKindName(InboxKind);
InboxKind parseInboxKind(std::string_view);

struct InboxTarget {
    std::string botId;
    InboxKind itemKind;
    Id itemId, waitId, taskId, targetRevision, targetGeneration;
    bool operator==(const InboxTarget&) const = default;
};

class Store;
class InboxStorage;
struct Person;

class Inbox {
public:
    explicit Inbox(Store&);

    Json inbox(const Person&, std::string_view bot = {}) const;
    Json decisionHistory(const Person&, std::string_view bot = {}) const;
    void decideInbox(const Person&, const InboxTarget&, std::string_view decision, const Json& data = Json::object());
private:
    InboxStorage& inbox_;
};

}
