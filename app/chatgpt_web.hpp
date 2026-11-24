#pragma once

#include "settings_access.hpp"
#include "flock/web.hpp"

#include <mutex>
#include <thread>

namespace flockv0::app {
class ChatGPTModel;

class ChatGPTWeb {
public:
    ChatGPTWeb(ChatGPTModel&, SettingsAccess&);
    void configure(WebCallbacks&);

private:
    Json status();
    bool signIn(const Person&, bool reconsent, bool switchAccount);
    std::optional<bool> signOut();

    ChatGPTModel& model_;
    SettingsAccess& access_;
    std::mutex mutex_;
    bool running_ = false;
    unsigned attempts_ = 0;
    std::string error_;
    std::jthread worker_;
};
}
