#pragma once

#include "flock/core.hpp"

namespace flockv0 {

enum class T3ConnectionMode { Full, Consult };
std::string_view t3ConnectionModeName(T3ConnectionMode);

struct T3Connection {
    std::string id, environmentId, personId;
    T3ConnectionMode mode;
};

class T3Connections {
public:
    explicit T3Connections(Store& store) : store_(store) {}
    std::string connect(std::string_view environment, std::string_view person, T3ConnectionMode, unsigned port);
    bool disconnect(std::string_view environment);
    void providerRemoved(std::string_view environment);
    std::optional<T3Connection> authenticate(std::string_view token) const;
    std::vector<T3Connection> list(std::string_view person) const;
    unsigned port() const;
    bool configured() const;

private:
    Store& store_;
};

}
