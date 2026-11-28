#include "flock/settings_access.hpp"
#include "fixtures.hpp"
#include "flock/base64.hpp"
#include "flock/json.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/store.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using namespace flockv0;
using namespace flockv0::app::test;

std::string authorization(const Json& password) {
    return "Basic " + encodeBase64(password.at("person").get<std::string>() + ":" + password.at("password").get<std::string>());
}

void accessLifecycle() {
    Sandbox home;
    Store store(home.path / "flock.sqlite3");
    SettingsAccess access(store, nullptr);
    auto identity = access.identities();
    check(!access.enabled() && identity.authenticate("") == localOwner, "Local setup does not require an account");
    check(access.view(localOwner).at("mode") == "local", "Local setup status");
    rejects<AccessDenied>([&] { access.change({"member", false}, {{"operation", "prepare"}}); },
                          "An ordinary user prepared the initial administrator");

    const auto first = access.change(localOwner, {{"operation", "prepare"}}).at("credential");
    const auto firstPassword = first.at("password").get<std::string>();
    const auto firstHeader = authorization(first);
    check(identity.authenticate(firstHeader).id.empty(), "An unconfirmed credential signed in");
    check(access.view(localOwner).dump().find(firstPassword) == std::string::npos, "The setup status returned a password");

    Store otherStore(home.path / "flock.sqlite3");
    SettingsAccess other(otherStore, nullptr);
    rejects<Conflict>([&] { other.change(localOwner, {{"operation", "activate"}, {"password", "not it"}}); },
                      "Incorrect bootstrap password unlocked the installation");
    other.change(localOwner, {{"operation", "activate"}, {"password", firstPassword}});
    check(access.enabled() && Policy{store}.requiresIdentity(), "Activation did not require identity across connections");
    check(identity.authenticate("").id.empty() && identity.authenticate(firstHeader) == localOwner,
          "Activation did not exchange local access for verified credentials");
    rejects<Conflict>([&] { access.change(localOwner, {{"operation", "role"}, {"person", localOwner.id}, {"adminRoot", false}}); },
                      "The last administrator was demoted");

    const auto member = other.change(localOwner, {{"operation", "invite"}, {"person", "member"}, {"adminRoot", false}}).at("credential");
    const auto memberHeader = authorization(member);
    check(identity.authenticate(memberHeader) == Person{"member", false}, "New user credentials are unavailable");
    check(access.view(localOwner).dump().find(member.at("password").get<std::string>()) == std::string::npos,
          "The account list returned a password");
    other.change(localOwner, {{"operation", "role"}, {"person", "member"}, {"adminRoot", true}});
    const auto administrator = identity.authenticate(memberHeader);
    check(administrator == Person{"member", true}, "Promotion is unavailable to other connections");
    check(identity.resolveRoot("member") == true, "Queued work cannot see the new role");
    int credentialCommits = 0;
    other.commitIfAdministrator(administrator, [&] { ++credentialCommits; });
    check(credentialCommits == 1, "An authorized account could not commit a credential change");
    rejects<AccessDenied>([&] { other.change(administrator, {{"operation", "rotate"}, {"person", localOwner.id}}); },
                          "A delegated administrator replaced the installation owner's credential");

    const auto rotated = access.change(localOwner, {{"operation", "rotate"}, {"person", "member"}}).at("credential");
    const auto newHeader = authorization(rotated);
    check(identity.authenticate(memberHeader) == administrator && identity.authenticate(newHeader).id.empty(),
          "An unconfirmed password replacement changed access");
    other.change(localOwner, {{"operation", "activate"}, {"person", "member"}, {"password", rotated.at("password")}});
    check(identity.authenticate(memberHeader).id.empty() && identity.authenticate(newHeader) == administrator,
          "Password replacement was not published across connections");

    access.change(localOwner, {{"operation", "remove"}, {"person", "member"}});
    check(identity.authenticate(newHeader).id.empty() && !identity.resolveRoot("member"), "Revoked credentials still grant access");
    rejects<AccessDenied>([&] { other.commitIfAdministrator(administrator, [&] { ++credentialCommits; }); },
                          "A revoked administrator was able to commit a credential change");
    check(credentialCommits == 1, "The credential change ran after the administrator was revoked");
    rejects<AccessDenied>([&] { other.change(administrator, {{"operation", "invite"}, {"person", "another"}, {"adminRoot", false}}); },
                          "A revoked administrator changed settings with an earlier authorization");
    rejects<Conflict>([&] { access.change(localOwner, {{"operation", "invite"}, {"person", "member"}, {"adminRoot", false}}); },
                      "Reusing a revoked identity can inherit its existing Bot membership");

    Store reopened(home.path / "flock.sqlite3");
    SettingsAccess afterRestart(reopened, nullptr);
    check(afterRestart.identities().authenticate(firstHeader) == localOwner && afterRestart.identities().authenticate("").id.empty(),
          "Saved identity was lost on restart");
}

void externalAccess() {
    Sandbox home;
    const auto identities = home.path / "identities.json";
    constexpr auto header = "Bearer fixture-read-only";
    constexpr auto memberHeader = "Bearer fixture-member-only";
    std::ofstream(identities) << Json::array({{{"person", "admin"}, {"adminRoot", true}, {"authorization", header}},
                                              {{"person", "admin"}, {"authorization", memberHeader}}})
                                     .dump();
    std::filesystem::permissions(identities, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    Store store(home.path / "flock.sqlite3");
    SettingsAccess access(store, identities.c_str());
    const auto admin = access.identities().authenticate(header);
    check(access.identities().resolveRoot(admin.id) == false && access.canManageInstallation(admin) &&
              !access.canManageInstallation(access.identities().authenticate(memberHeader)),
          "Installation setup did not use the role of the authenticated credential");
    const auto status = access.view(admin);
    check(admin == Person{"admin", true} && status.at("mode") == "external" && status.at("users").at(0).at("person") == "admin" &&
              status.dump().find(header) == std::string::npos,
          "Host-managed access status did not show safe account metadata");
    rejects<Conflict>([&] { access.change(admin, {{"operation", "invite"}, {"person", "new"}, {"adminRoot", false}}); },
                      "The UI overwrote the authority of the identity file");

    Store reopened(home.path / "flock.sqlite3");
    SettingsAccess missingFile(reopened, nullptr);
    check(missingFile.enabled() && missingFile.identities().authenticate("").id.empty(),
          "Removing the external settings fell back to local owner");
}

}

int main() try {
    accessLifecycle();
    externalAccess();
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
