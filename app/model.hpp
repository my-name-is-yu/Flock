#pragma once

#include "flock/core.hpp"

#include <iosfwd>
#include <stop_token>

namespace flockv0::app {
using CommitModelLogin = std::function<void(const std::function<void()>&)>;

class ChatGPTModel final : public Model {
public:
    explicit ChatGPTModel(std::filesystem::path directory);
    ModelTurn decide(const ModelInput&) override;
    Json login(bool printUrl, bool reconsent);
    Json loginWeb(bool reconsent, std::stop_token, bool switchAccount, const CommitModelLogin&);
    Json signOut();
    Json status();
    Json webStatus();
    Json models();
private:
    std::filesystem::path directory_;
    Json call(std::string_view operation, const Json& input = Json::object(), std::stop_token stop = {},
              const CommitModelLogin& commit = {});
};

Json responseRequest(const ModelInput&);
ModelTurn responseDecision(const Json& output, const Json& request);
bool runModelAcceptance(Model&, const std::string& slug, std::ostream&);
}
