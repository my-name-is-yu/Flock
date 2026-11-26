#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Database;
class SecretRedactor;
struct SecretRef;

class SecretStorage {
public:
    SecretStorage(Database&, const std::filesystem::path& stateDirectory);
    ~SecretStorage();
    SecretStorage(const SecretStorage&) = delete;
    SecretStorage& operator=(const SecretStorage&) = delete;

    void insert(const SecretRef&, std::string_view value);
    std::string read(const SecretRef&) const;
    void protectGitCredential(std::string value);
    std::size_t gitCredentialCount() const;
    SecretRedactor redactor(std::string_view bot, Json* revision = nullptr) const;
    SecretRedactor sharedRedactor() const;
private:
    Database& db_;
    int stateDirectory_;
    std::vector<std::string> gitCredentials_;
    Id gitRedactionEpoch_ = 0;
};
}
