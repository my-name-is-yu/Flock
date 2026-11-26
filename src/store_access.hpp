#pragma once

#include "storage.hpp"

namespace flockv0 {
class StoreAccess {
public:
    using Impl = Store::Impl;

    static Impl& get(Store& store) { return *store.p_; }
};
}
