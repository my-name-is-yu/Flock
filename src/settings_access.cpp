#include "flock/policy_rules.hpp"
#include "flock/digest.hpp"
#include "flock/settings_access.hpp"
#include "flock/decision_errors.hpp"
#include "database.hpp"
#include "flock/identity.hpp"
#include "flock/base64.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace flockv0 {
namespace {

constexpr Seconds confirmationWindow = 10 * 60;
constexpr std::size_t maximumPersonLength = 80;
constexpr std::size_t passwordBytes = 24;
constexpr std::size_t passwordLength = passwordBytes * 2;
constexpr std::string_view basicPrefix = "Basic ";

enum class Mode { local, managed, unavailable };

std::string newPassword() {
    std::array<unsigned char, passwordBytes> random{};
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) throw Conflict("Could not generate a sign-in password");
    return encodeHex(random);
}

// Passwords are generated from 192 bits of randomness and never chosen by callers.
std::string passwordDigest(std::string_view password) {
    return sha256Hex(password);
}

bool matches(std::string_view password, std::string_view digest) {
    if (password.size() != passwordLength || digest.size() != 64) return false;
    const auto candidate = passwordDigest(password);
    return CRYPTO_memcmp(candidate.data(), digest.data(), candidate.size()) == 0;
}

bool validPerson(std::string_view person) {
    return !person.empty() && person.size() <= maximumPersonLength && person != "." && person != ".." &&
           person.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._@-") == person.npos;
}

std::string requirePerson(const Json& args) {
    const auto person = args.at("person").get<std::string>();
    if (!validPerson(person)) throw Conflict("Use letters, numbers, periods, @, underscores or hyphens for the username");
    return person;
}

Mode accessMode(const auto& db) {
    if (db.query("SELECT 1 FROM managed_web_users LIMIT 1").next()) return Mode::managed;
    return db.query("SELECT 1 FROM web_identity_required").next() ? Mode::unavailable : Mode::local;
}

std::optional<bool> currentRole(const auto& db, std::string_view person) {
    auto user = db.query("SELECT admin FROM managed_web_users WHERE person=? AND revoked=0", {person});
    return user.next() ? std::optional{user.number(0) != 0} : std::nullopt;
}

void requireAdministrator(const auto& db, const Person& actor, Mode mode) {
    if (!actor.adminRoot || actor.id.empty()) throw DecisionDenied{};
    if (mode == Mode::local && actor == localOwner) return;
    if (mode == Mode::managed && currentRole(db, actor.id).value_or(false)) return;
    throw DecisionDenied{};
}

Json credential(std::string_view person, std::string_view password) {
    return {{"person", person}, {"password", password}};
}

Json preparePassword(auto& db, std::string_view person, Seconds now) {
    const auto password = newPassword();
    const auto expires = now + confirmationWindow;
    auto result = Json{{"credential", credential(person, password)}, {"expiresAt", expires}};
    db.exec(R"SQL(
INSERT INTO managed_web_pending VALUES(?,?,?)
ON CONFLICT(person) DO UPDATE SET verifier=excluded.verifier,expires_at=excluded.expires_at
)SQL",
            {person, passwordDigest(password), expires});
    return result;
}

void activatePassword(auto& db, std::string_view person, std::string_view password, Seconds now, bool first) {
    auto pending = db.query("SELECT verifier FROM managed_web_pending WHERE person=? AND expires_at>?", {person, now});
    if (!pending.next() || !matches(password, pending.string(0)))
        throw Conflict("The password is incorrect or expired. Create a new password to try again.");
    const auto digest = pending.string(0);
    if (first) {
        db.exec("INSERT INTO managed_web_users VALUES(?,?,1,0)", {person, digest});
        db.exec("INSERT OR IGNORE INTO web_identity_required VALUES(1)");
    } else {
        db.exec("UPDATE managed_web_users SET verifier=? WHERE person=? AND revoked=0", {digest, person});
    }
    db.exec("DELETE FROM managed_web_pending WHERE person=?", {person});
}

void keepAdministrator(const auto& db, std::string_view person, bool admin) {
    if (admin && !db.query("SELECT 1 FROM managed_web_users WHERE revoked=0 AND admin=1 AND person<>?", {person}).next())
        throw Conflict("Flock needs at least one administrator");
}

}

SettingsAccess::SettingsAccess(Store& store, const char* externalFile) : store_(store) {
    auto& db = Database::of(store_);
    std::lock_guard lock(db.mutex);
    if (externalFile) {
        Transaction transaction(db.db);
        if (accessMode(db) == Mode::managed) throw Conflict("Remove the external Web identity file setting to use managed users");
        external_ = loadIdentities(store_, externalFile);
        PolicyRules{store_}.requireWebIdentity();
        transaction.commit();
    }
    WebIdentity(store_).setWebIdentityResolver(identities().resolveRoot, !external_.authenticate);
}

bool SettingsAccess::canManageInstallation(const Person& actor) const {
    if (!Policy{store_}.allows(actor, Action::rules_manage, "installation")) return false;
    // Host-managed identities authenticate a specific credential. The
    // conservative per-person resolver applies to queued work.
    return external_.authenticate || resolve(store_, actor.id).value_or(false);
}

void SettingsAccess::commitIfAdministrator(const Person& actor, const std::function<void()>& commit) {
    auto& db = Database::of(store_);
    std::lock_guard lock(db.mutex);
    Transaction transaction(db.db);
    if (!canManageInstallation(actor)) throw DecisionDenied{};
    commit();
    transaction.commit();
}

bool SettingsAccess::enabled() const {
    const auto& db = Database::of(store_);
    std::lock_guard lock(db.mutex);
    return external_.authenticate || accessMode(db) != Mode::local;
}

std::optional<bool> SettingsAccess::resolve(Store& store, std::string_view person) {
    const auto& db = Database::of(store);
    std::lock_guard lock(db.mutex);
    const auto mode = accessMode(db);
    if (mode == Mode::managed) return currentRole(db, person);
    if (mode == Mode::local && person == localOwner.id) return true;
    return std::nullopt;
}

Person SettingsAccess::authenticate(Store& store, std::string_view authorization) {
    const auto& db = Database::of(store);
    std::lock_guard lock(db.mutex);
    if (authorization.empty()) return accessMode(db) == Mode::local ? localOwner : Person{};
    if (!authorization.starts_with(basicPrefix) || accessMode(db) != Mode::managed) return {};
    std::string decoded;
    try {
        decoded = decodeBase64(authorization.substr(basicPrefix.size()), maximumPersonLength + passwordLength + 1);
    } catch (const std::invalid_argument&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
    const auto split = decoded.find(':');
    if (split == decoded.npos) return {};
    const auto name = std::string_view(decoded).substr(0, split);
    const auto password = std::string_view(decoded).substr(split + 1);
    if (!validPerson(name)) return {};
    auto user = db.query("SELECT admin,verifier FROM managed_web_users WHERE person=? AND revoked=0", {name});
    if (!user.next() || !matches(password, user.string(1))) return {};
    return {std::string(name), user.number(0) != 0};
}

ConfiguredIdentities SettingsAccess::identities() const {
    if (external_.authenticate) return external_;
    auto root = [store = &store_](std::string_view person) { return resolve(*store, person); };
    return {[store = &store_](std::string_view authorization) { return authenticate(*store, authorization); }, root,
            [root](std::string_view person) -> std::optional<Person> {
                const auto admin = root(person);
                if (!admin) return std::nullopt;
                return Person{std::string(person), *admin};
            }};
}

Json SettingsAccess::view(const Person& actor) const {
    const auto& db = Database::of(store_);
    std::lock_guard lock(db.mutex);
    auto users = Json::array();
    if (external_.authenticate) {
        if (!actor.adminRoot) throw DecisionDenied{};
        for (const auto& person : external_.users) users.push_back({{"person", person.id}, {"adminRoot", person.adminRoot}});
        return {{"mode", "external"}, {"users", users}};
    }
    const auto mode = accessMode(db);
    requireAdministrator(db, actor, mode);
    const auto now = unixTime();
    if (mode == Mode::local) {
        auto result = Json{{"mode", "local"}, {"users", users}};
        auto pending = db.query("SELECT expires_at FROM managed_web_pending WHERE person=? AND expires_at>?", {localOwner.id, now});
        if (pending.next()) result["pendingUntil"] = pending.number(0);
        return result;
    }
    auto rows = db.query(R"SQL(
SELECT u.person,u.admin,coalesce(p.expires_at,0) FROM managed_web_users u
LEFT JOIN managed_web_pending p ON p.person=u.person AND p.expires_at>?
WHERE u.revoked=0 ORDER BY u.person
)SQL",
                         {now});
    while (rows.next()) {
        auto user = Json{{"person", rows.string(0)}, {"adminRoot", rows.number(1) != 0}};
        if (const auto until = rows.number(2)) user["pendingUntil"] = until;
        users.push_back(std::move(user));
    }
    return {{"mode", "managed"}, {"currentPerson", actor.id}, {"users", std::move(users)}};
}

Json SettingsAccess::change(const Person& actor, const Json& args) {
    if (external_.authenticate) throw Conflict("Web access is managed by the host's identity file");
    auto& db = Database::of(store_);
    std::lock_guard lock(db.mutex);
    Transaction transaction(db.db);
    const auto mode = accessMode(db);
    requireAdministrator(db, actor, mode);
    const auto operation = args.at("operation").get<std::string>();
    const auto now = unixTime();
    Json result{{"ok", true}};
    if (mode == Mode::local) {
        if (operation == "prepare")
            result = preparePassword(db, localOwner.id, now);
        else if (operation == "activate")
            activatePassword(db, localOwner.id, args.at("password").get<std::string>(), now, true);
        else
            throw Conflict("Enable shared access before changing users");
    } else {
        const auto target = requirePerson(args);
        // The installation owner's host access cannot be delegated by changing
        // the owner's role or password from another administrator's account.
        if (target == localOwner.id) {
            if (actor != localOwner) throw DecisionDenied{};
            if (operation == "role" || operation == "remove") throw Conflict("The installation owner must retain administrator access");
        }
        if (operation == "invite") {
            if (db.query("SELECT 1 FROM managed_web_users WHERE person=?", {target}).next())
                throw Conflict("That username is already in use or has been revoked");
            const auto admin = args.at("adminRoot").get<bool>();
            const auto password = newPassword();
            result["credential"] = credential(target, password);
            db.exec("INSERT INTO managed_web_users VALUES(?,?,?,0)", {target, passwordDigest(password), admin ? 1 : 0});
        } else {
            const auto admin = currentRole(db, target);
            if (!admin) throw Conflict("That user no longer has access");
            if (operation == "role") {
                const bool newRole = args.at("adminRoot").get<bool>();
                if (!newRole) keepAdministrator(db, target, *admin);
                db.exec("UPDATE managed_web_users SET admin=? WHERE person=?", {newRole ? 1 : 0, target});
            } else if (operation == "remove") {
                keepAdministrator(db, target, *admin);
                if (db.query("SELECT 1 FROM t3_connector WHERE person_id=? "
                             "AND EXISTS(SELECT 1 FROM t3_connected_environments)",
                             {target})
                        .next())
                    throw Conflict("Disconnect T3 before revoking the account that connected it");
                db.exec("DELETE FROM managed_web_pending WHERE person=?", {target});
                db.exec("UPDATE managed_web_users SET verifier='',admin=0,revoked=1 WHERE person=?", {target});
            } else if (operation == "rotate")
                result = preparePassword(db, target, now);
            else if (operation == "activate")
                activatePassword(db, target, args.at("password").get<std::string>(), now, false);
            else
                throw Conflict("Unknown access operation");
        }
    }
    transaction.commit();
    return result;
}

}
