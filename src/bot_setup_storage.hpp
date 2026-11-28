#pragma once

#include "flock/bot_setup.hpp"
#include "flock/intake.hpp"
#include "policy_origin.hpp"

#include <optional>
#include <utility>

namespace flockv0 {
class Database;
class SecretStorage;
struct TaskData;

class BotSetupStorage {
public:
    BotSetupStorage(Store& store, Database& database, const SecretStorage& secrets, SaveBotSetup save, std::function<Json()> catalog)
        : store(store), database(database), secrets(secrets), save(std::move(save)), loadCatalog(std::move(catalog)) {}

    explicit operator bool() const { return bool(save); }
    void validate(std::string_view bot, const Json& patch) const;
    Json modelCatalog(const Json& patch) const;
    std::optional<Json> recorded(std::string_view operation);
    Json apply(std::string_view bot, std::string_view operation, const Json&, const Json& catalog, Seconds now, Id task = 0,
               Id message = 0);
    std::optional<Intake> liveIntake(const IntakeResolver&, std::string_view bot, std::string_view text) const;
    void undo(const Person&, std::string_view bot, Id event);
private:
    Store& store;
    Database& database;
    const SecretStorage& secrets;
    SaveBotSetup save;
    std::function<Json()> loadCatalog;
};
}
