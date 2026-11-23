#pragma once

#include "flock/identity.hpp"

namespace flockv0 {

// Manages the Web identities for one installation. The resolver reads current
// privileges from the Store so queued work observes account changes too.
class SettingsAccess {
public:
    SettingsAccess(Store&, const std::filesystem::path& home, const char* externalFile);
    bool enabled() const;
    ConfiguredIdentities identities() const;
    bool canManageInstallation(const Person&) const;
    void commitIfAdministrator(const Person&, const std::function<void()>&);
    Json view(const Person&) const;
    Json change(const Person&, const Json&);

private:
    static std::optional<bool> resolve(Store&, std::string_view person);
    static Person authenticate(Store&, std::string_view authorization);
    Store& store_;
    ConfiguredIdentities external_;
};

}
