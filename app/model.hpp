#pragma once

#include "flock/core.hpp"

#include <iosfwd>

namespace flockv0::app {
class ChatGPTModel final : public Model {
public:
    explicit ChatGPTModel(std::filesystem::path directory);
    Decision decide(const ModelInput&) override;
    Json login(bool printUrl, bool reconsent);
    Json status();
    Json models();
private:
    std::filesystem::path directory_;
    Json call(std::string_view operation, const Json& input = Json::object());
};

Json responseRequest(const ModelInput&);
Decision responseDecision(const Json& output, const std::vector<std::string>& allowed);
bool runModelAcceptance(Model&, const std::string& slug, std::ostream&);
} // namespace flockv0::app
