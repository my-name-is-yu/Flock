#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace flockv0 {
class Model;
class Store;
class T3;
struct RuntimeOptions;

class Runtime {
public:
    using SoulLoader = std::function<std::string(std::string_view)>;
    Runtime(Store&, Model&, T3&, SoulLoader);
    Runtime(Store&, Model&, T3&, SoulLoader, RuntimeOptions);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    void runConversations();
    // tick() completes a step for each eligible project. tickConcurrent() schedules
    // eligible projects independently and returns immediately.
    void tick();
    void tickConcurrent();
    // Let admitted work settle while stopping further dispatch.
    void requestStop();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
