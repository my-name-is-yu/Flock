#pragma once

#include "flock/core.hpp"

namespace flockv0 {

enum class T3ConnectionMode { Full, Consult };
std::string_view t3ConnectionModeName(T3ConnectionMode);

struct T3Connection {
    std::string id, personId;
    T3ConnectionMode mode;
};

struct T3ConnectionSettings {
    T3ConnectionMode mode;
    unsigned port;
    std::vector<std::string> environments;
};

class T3Connections {
public:
    explicit T3Connections(Store& store) : store_(store) {}
    // Returns a new bearer only when provider configuration needs to change.
    std::string connect(std::string_view environment, std::string_view person, T3ConnectionMode, unsigned port,
                        const std::function<void(std::string_view)>& install = {}, bool refreshProviders = false);
    bool ownsBearer(std::string_view token) const;
    bool disconnect(std::string_view environment);
    void providerRemoved();
    std::optional<T3Connection> authenticate(std::string_view token) const;
    std::optional<T3ConnectionSettings> settings(std::string_view person) const;
    unsigned port() const;
    bool configured() const;

private:
    Store& store_;
};

}
