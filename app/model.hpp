#pragma once

#include "chatgpt.hpp"

#include <iosfwd>
#include <stop_token>

namespace flockv0::app {
using CommitModelLogin = std::function<void(const std::function<void()>&)>;

class ChatGPTModel final : public Model {
public:
    explicit ChatGPTModel(std::filesystem::path directory);
    ChatGPTModel(std::filesystem::path directory, chatgpt::HttpTransport, chatgpt::BrowserOpener = chatgpt::openBrowser);
    ModelTurn decide(const ModelInput&) override;
    Json respond(const Json& request, std::size_t inspectedImageBytes = 0);
    Json login(bool printUrl, bool reconsent);
    Json loginWeb(bool reconsent, std::stop_token, bool switchAccount, const CommitModelLogin&);
    Json signOut();
    Json status();
    Json webStatus();
    Json models();
private:
    std::filesystem::path directory_;
    chatgpt::HttpTransport transport_;
    chatgpt::BrowserOpener opener_;
    std::string accessToken();
    Json sendResponse(const Json&, std::size_t inspectedImageBytes, bool recordFlockContract);
};

Json responseRequest(const ModelInput&);
ModelTurn responseDecision(const Json& output, const Json& request);
bool runModelAcceptance(Model&, const std::string& slug, std::ostream&);
}
