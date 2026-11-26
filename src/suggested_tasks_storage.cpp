#include "suggested_tasks_storage.hpp"
#include "database.hpp"
#include "flock/secrets.hpp"
#include "flock/suggested_tasks.hpp"
#include "record_decode.hpp"
#include "secret_storage.hpp"
#include "task_storage.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace flockv0 {
namespace {
constexpr std::array statuses{
    std::pair{SuggestedTaskStatus::suggested, std::string_view{"suggested"}},
    std::pair{SuggestedTaskStatus::started, std::string_view{"started"}},
    std::pair{SuggestedTaskStatus::dismissed, std::string_view{"dismissed"}},
};

std::string_view requestKey(std::string_view request) {
    constexpr std::string_view whitespace = " \t\n\r";
    const auto begin = request.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) return {};
    return request.substr(begin, request.find_last_not_of(whitespace) - begin + 1);
}

Json encodeDestinations(const StartDestinations& destinations) {
    return {{"primary", runTargetJson({{}, destinations.primary})},
            {"targets", runTargetsJson(destinations.targets)},
            {"commit", destinations.commit}};
}

StartDestinations decodeDestinations(Id itemId, std::string_view stored) {
    const auto id = std::to_string(itemId);
    const RecordDecoder row{"suggested_tasks", id, "destinations"};
    const auto value = row.parse(stored);
    return row.decodeClosed(value, [](const Json& data, const RecordDecoder& decoder) {
        StartDestinations destinations{.primary =
                                           decoder.nested("primary").decodeClosed(decoder.require(data, "primary"), decodeRunTarget).route,
                                       .targets = {},
                                       .commit = decoder.get<std::string>(data, "commit")};
        const auto targets = decoder.nested("targets");
        for (const auto& target : targets.array(decoder.require(data, "targets")))
            destinations.targets.push_back(targets.decodeClosed(target, decodeRunTarget));
        return destinations;
    });
}

using DestinationKey = std::set<std::pair<std::string, std::string>>;

DestinationKey destinationKey(const StartDestinations& destinations) {
    if (destinations.targets.empty()) return {{destinations.primary.environmentId, destinations.primary.projectId}};
    DestinationKey key;
    for (const auto& [name, route] : destinations.targets) key.emplace(route.environmentId, route.projectId);
    return key;
}

bool matchesActiveTask(Database& database, std::string_view bot, std::string_view request, const DestinationKey& targets) {
    auto tasks = database.query(R"SQL(
SELECT id,environment_id,project_id FROM tasks
WHERE bot_id=? AND state NOT IN ('completed','cancelled') AND trim(request,char(32,9,10,13))=? COLLATE NOCASE
)SQL",
                                {bot, request});
    while (tasks.next()) {
        auto destinations =
            database.query("SELECT environment_id,project_id FROM task_run_destinations WHERE task_id=?", {tasks.number(0)});
        DestinationKey key;
        while (destinations.next()) key.emplace(destinations.string(0), destinations.string(1));
        if (key.empty()) key.emplace(tasks.string(1), tasks.string(2));
        if (key == targets) return true;
    }
    return false;
}

void journalChange(Database& database, std::string_view bot, Id cardId, Id itemId, SuggestedTaskStatus status) {
    database.writeJournal(std::nullopt, JournalKind::suggested_tasks_changed, {{"itemId", itemId}, {"status", enumName(statuses, status)}},
                          {.messageId = cardId}, bot);
}

std::string_view gateway(const TaskRoute& route) {
    return route.gatewayEnvironmentId.empty() ? route.environmentId : route.gatewayEnvironmentId;
}

bool matchesEnvironment(const TaskRoute& pinned, const TaskRoute& target) {
    return pinned.environmentId == target.environmentId && gateway(pinned) == gateway(target);
}
}

std::size_t SuggestionStorage::suggest(std::string_view bot, Id sourceMessageId, std::optional<Id> sourceTaskId,
                                       std::span<const SuggestedTaskInput> items) {
    if (items.empty()) return 0;
    auto count = database_.query("SELECT count(*) FROM suggested_tasks WHERE bot_id=? AND source_message_id=?", {bot, sourceMessageId});
    count.next();
    const auto existing = static_cast<std::size_t>(count.number(0));
    count.next();
    if (existing >= maxSuggestedTasks) return 0;
    const auto scrub = secrets_.redactor(bot);
    std::size_t inserted = 0;
    for (const auto& item : items) {
        if (existing + inserted >= maxSuggestedTasks) break;
        const auto request = scrub.text(item.request);
        const auto key = requestKey(request);
        const auto targets = destinationKey(item.destinations);
        const auto targetKey = Json(targets).dump();
        if (matchesActiveTask(database_, bot, key, targets)) continue;
        if (database_
                .query(R"SQL(
SELECT 1 FROM suggested_tasks
WHERE bot_id=?1 AND request_key=?2 COLLATE NOCASE AND destination_key=?3
  AND (status='dismissed' OR source_message_id=?4)
LIMIT 1
)SQL",
                       {bot, key, targetKey, sourceMessageId})
                .next())
            continue;
        const auto destinations = encodeDestinations(item.destinations);
        if (scrub.containsProtected(destinations)) continue;
        database_.exec(R"SQL(
INSERT INTO suggested_tasks(bot_id,source_message_id,source_task_id,request,request_key,done_when,reason,destination_label,
                            destinations,destination_key,status)
VALUES(?,?,?,?,?,?,?,?,?,?,'suggested')
)SQL",
                       {bot, sourceMessageId, sourceTaskId ? Json(*sourceTaskId) : Json(nullptr), request, key, scrub.text(item.doneWhen),
                        scrub.text(item.reason), scrub.text(item.destinationLabel), destinations, targetKey});
        ++inserted;
    }
    return inserted;
}

void SuggestionStorage::reanchor(std::string_view bot, Id from, Id to) {
    database_.exec("UPDATE suggested_tasks SET source_message_id=? WHERE bot_id=? AND source_message_id=?", {to, bot, from});
}

std::vector<SuggestedTask> SuggestionStorage::forCard(std::string_view bot, Id sourceMessageId) const {
    auto rows = database_.query(R"SQL(
SELECT s.id,s.request,s.done_when,s.reason,s.destination_label,s.destinations,s.status,s.task_id,s.source_task_id,
       (SELECT j.id FROM journal j WHERE j.task_id=s.task_id AND j.kind='request' ORDER BY j.id LIMIT 1)
FROM suggested_tasks s JOIN journal card ON card.id=s.source_message_id
WHERE s.bot_id=? AND card.id=? AND card.kind IN ('conversation_reply','completed')
ORDER BY s.id
)SQL",
                                {bot, sourceMessageId});
    const auto optionalId = [&](int col) -> std::optional<Id> { return rows.null(col) ? std::nullopt : std::optional{rows.number(col)}; };
    std::vector<SuggestedTask> items;
    while (rows.next()) {
        const auto id = rows.number(0);
        items.push_back({id,
                         {rows.string(1), rows.string(2), rows.string(3), rows.string(4), decodeDestinations(id, rows.string(5))},
                         enumValue(statuses, rows.string(6)).value(),
                         optionalId(7),
                         optionalId(9),
                         optionalId(8)});
    }
    return items;
}

bool SuggestionStorage::dismiss(std::string_view bot, Id itemId) {
    auto updated = database_.query(R"SQL(
UPDATE suggested_tasks SET status='dismissed'
WHERE id=? AND bot_id=? AND status='suggested'
  AND EXISTS(SELECT 1 FROM journal WHERE id=source_message_id AND kind IN ('conversation_reply','completed'))
RETURNING source_message_id
)SQL",
                                   {itemId, bot});
    if (!updated.next()) return false;
    const auto cardId = updated.number(0);
    updated.next();
    journalChange(database_, bot, cardId, itemId, SuggestedTaskStatus::dismissed);
    return true;
}

void SuggestionStorage::started(std::string_view bot, Id sourceMessageId, Id itemId, Id taskId, std::string_view request) {
    const auto scrub = secrets_.redactor(bot);
    auto updated = database_.query(R"SQL(
UPDATE suggested_tasks SET status='started',task_id=?,request=?
WHERE bot_id=? AND source_message_id=? AND id=? AND status='suggested'
RETURNING id
)SQL",
                                   {taskId, scrub.text(request), bot, sourceMessageId, itemId});
    if (!updated.next()) throw Conflict("That suggestion is no longer available");
    updated.next();
    journalChange(database_, bot, sourceMessageId, itemId, SuggestedTaskStatus::started);
}

bool SuggestionStorage::coversLaunch(Id taskId, const TaskRoute& route) const {
    auto row = database_.query("SELECT id,destinations FROM suggested_tasks WHERE task_id=?", {taskId});
    if (!row.next()) return false;
    const auto destinations = decodeDestinations(row.number(0), row.string(1));
    if (destinations.targets.empty())
        return matchesEnvironment(destinations.primary, route) &&
               (destinations.primary.projectId.empty() || destinations.primary.projectId == route.projectId);
    return std::ranges::any_of(destinations.targets, [&](const auto& target) {
        return matchesEnvironment(target.route, route) && target.route.projectId == route.projectId;
    });
}
}
