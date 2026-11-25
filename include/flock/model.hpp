#pragma once

#include "flock/decision.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace flockv0 {
struct BotNotes {
    std::string soul, memory;
};
struct BotNotesUnavailable : Conflict {
    using Conflict::Conflict;
};

struct ModelTurn {
    std::vector<Decision> calls;
    Json usage = Json::object();
    std::map<std::size_t, std::string> rejections{}; // Adapter validation errors by zero-based call index.
};
struct ModelImage {
    Id messageId;
    std::string attachmentId, mimeType, dataBase64;
};
struct ModelInput {
    Json task, event, history;
    BotNotes notes;
    std::vector<std::string> tools;
    std::string feedback;
    Json evidence = Json::array();
    Json config = Json::object(); // Adapter settings only. Never serialize into the prompt.
    bool needsDoneWhen = false, canForwardUserMessage = false;
    std::vector<ModelImage> images{};
};
class Model {
public:
    virtual ~Model() = default;
    virtual ModelTurn decide(const ModelInput&) = 0;
};
}
