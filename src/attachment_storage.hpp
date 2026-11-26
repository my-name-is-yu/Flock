#pragma once

#include "flock/json.hpp"
#include "flock/types.hpp"

#include <string_view>
#include <vector>

namespace flockv0 {
class Database;
class Statement;
struct ModelImage;

class AttachmentStorage {
public:
    explicit AttachmentStorage(Database& db) : db_(db) {}
    void saveAttachments(Id, const Json&);
    Json inspectAttachment(std::string_view bot, Id throughMessage, const Json&);
    Json attachmentViews(Id) const;
    std::vector<ModelImage> inspectedImages(std::string_view bot, Id message, const Json& steps) const;
    bool hasAttachment(std::string_view bot, Id throughMessage, const Json& args) const;
private:
    Statement scopedAttachment(std::string_view bot, Id throughMessage, const Json& args) const;
    Database& db_;
};
}
