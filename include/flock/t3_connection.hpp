#pragma once

#include "flock/core.hpp"

namespace flockv0 {

struct T3Connection {
    std::string id, personId;
};

struct T3ConnectionSettings {
    unsigned port;
    std::vector<std::string> environments;
};

class T3Connections {
public:
    explicit T3Connections(Store& store) : store_(store) {}
    // Returns a new bearer only when provider configuration needs to change.
    std::string connect(std::string_view environment, std::string_view person, unsigned port);
    bool disconnect(std::string_view environment);
    void providerRemoved();
    std::optional<T3Connection> authenticate(std::string_view token) const;
    std::optional<T3ConnectionSettings> settings(std::string_view person, bool installationRoot = false) const;
    unsigned port() const;
    bool configured() const;

private:
    Store& store_;
};

}
