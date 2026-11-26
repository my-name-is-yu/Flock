#pragma once

#include <string>

namespace flockv0 {
class Store;
class GitStorage;
class SecretStorage;
struct GitWebhook;

class GitEvents {
public:
    explicit GitEvents(Store&);

    bool receiveGitWebhook(const GitWebhook&);
    void protectGitCredential(std::string value);
private:
    GitStorage& git_;
    SecretStorage& secrets_;
};
}
