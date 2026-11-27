#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
class Store;

struct T3Connection {
    std::string id, personId;
};

struct T3RemoteMachine {
    std::string id, name;
};

struct T3RemoteCredential {
    std::string id, bearerToken;
};

struct T3ConnectionSettings {
    unsigned port;
    std::vector<std::string> environments;
};

class T3Connections {
public:
    explicit T3Connections(Store& store) : store_(store) {}
    // Returns a new bearer only when provider configuration needs to change.
    std::string connect(std::string_view environment, std::string_view person, unsigned port,
                        const std::function<void(std::string_view)>& install = {}, bool refreshProviders = false);
    // Moves the local provider registration without changing its owner or connected environments.
    void rebindPort(unsigned port, const std::function<void(std::string_view)>& install);
    bool ownsBearer(std::string_view token) const;
    bool disconnect(std::string_view environment);
    bool providerCleanupPending() const;
    void providerRemoved();
    // Accepts the local provider bearer and the bearers issued to other machines.
    std::optional<T3Connection> authenticate(std::string_view token) const;
    // Lets Sessions on another machine reach Flock as the person who connected T3. The bearer is returned only here.
    T3RemoteCredential addRemoteMachine(std::string_view name);
    std::vector<T3RemoteMachine> remoteMachines() const;
    bool removeRemoteMachine(std::string_view id);
    std::optional<T3ConnectionSettings> settings(std::string_view person, bool installationRoot = false) const;
    unsigned port() const;
    bool configured() const;
private:
    Store& store_;
};

}
