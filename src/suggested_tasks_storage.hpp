#pragma once

#include "flock/types.hpp"
#include "task_routing.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Database;
class SecretStorage;

struct SuggestedTaskInput {
    std::string request, doneWhen, reason, destinationLabel;
    StartDestinations destinations;
};

enum class SuggestedTaskStatus { suggested, started, dismissed };

struct SuggestedTask {
    Id id;
    SuggestedTaskInput input;
    SuggestedTaskStatus status;
    std::optional<Id> taskId, taskEventId, sourceTaskId;
};

class SuggestionStorage {
public:
    SuggestionStorage(Database& database, const SecretStorage& secrets) : database_(database), secrets_(secrets) {}

    std::size_t suggest(std::string_view bot, Id sourceMessageId, std::optional<Id> sourceTaskId,
                        std::span<const SuggestedTaskInput> items);
    void reanchor(std::string_view bot, Id from, Id to);
    std::vector<SuggestedTask> forCard(std::string_view bot, Id sourceMessageId) const;
    bool dismiss(std::string_view bot, Id itemId);
    void started(std::string_view bot, Id sourceMessageId, Id itemId, Id taskId, std::string_view request);
    bool coversLaunch(Id taskId, const TaskRoute&) const;
private:
    Database& database_;
    const SecretStorage& secrets_;
};
}
