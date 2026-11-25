#include "chatgpt_web.hpp"
#include "model.hpp"
#include "flock/board_inbox.hpp"

namespace flockv0::app {
ChatGPTWeb::ChatGPTWeb(ChatGPTModel& model, SettingsAccess& access) : model_(model), access_(access) {}

void ChatGPTWeb::configure(WebCallbacks& callbacks) {
    callbacks.modelStatus = [this] { return status(); };
    callbacks.modelCatalog = [this] { return model_.models(); };
    callbacks.modelLogin = [this](const Person& actor, bool reconsent) { return signIn(actor, reconsent, false); };
    callbacks.modelSwitch = [this](const Person& actor) { return signIn(actor, false, true); };
    callbacks.modelLogout = [this] { return signOut(); };
}

Json ChatGPTWeb::status() {
    unsigned attempts;
    {
        std::lock_guard lock(mutex_);
        if (running_) return Json{{"signingIn", true}};
        attempts = attempts_;
    }
    Json result;
    try {
        result = model_.webStatus();
    } catch (const std::exception&) {
        result = {{"unavailable", true}};
    }
    std::lock_guard lock(mutex_);
    if (running_) return Json{{"signingIn", true}};
    if (attempts_ != attempts) result = {{"unavailable", true}};
    if (!error_.empty()) result["signInError"] = error_;
    return result;
}

bool ChatGPTWeb::signIn(const Person& actor, bool reconsent, bool switchAccount) {
    std::lock_guard lock(mutex_);
    if (running_) return false;
    worker_ = std::jthread([this, actor, reconsent, switchAccount](std::stop_token stop) {
        std::string error;
        try {
            model_.loginWeb(reconsent, stop, switchAccount, [&](const auto& commit) { access_.commitIfAdministrator(actor, commit); });
        } catch (const DecisionDenied&) {
            error = "The account that started sign-in can no longer manage Flock.";
        } catch (const ModelError& failure) {
            error = failure.what();
        } catch (const std::exception&) {
            error = "Could not sign in to ChatGPT.";
        }
        std::lock_guard lock(mutex_);
        running_ = false;
        error_ = std::move(error);
    });
    running_ = true;
    ++attempts_;
    error_.clear();
    return true;
}

std::optional<bool> ChatGPTWeb::signOut() {
    std::lock_guard lock(mutex_);
    if (running_) return std::nullopt;
    ++attempts_;
    const auto revoked = model_.signOut().at("revocationConfirmed").get<bool>();
    error_.clear();
    return revoked;
}
}
