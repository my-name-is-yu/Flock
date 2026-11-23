#include "conversation_storage.hpp"

#include <map>

namespace flockv0 {
using conversation_storage::text;
using conversation_storage::ToolError;

namespace {
constexpr std::size_t artifactSize = 256 * 1024;
constexpr std::string_view imageMarker = "{{flock-image:";
constexpr std::string_view diagramKind = "html_diagram";
}

bool conversation::canPublishArtifact(const Message& message) {
    using conversation::PrincipalSource;
    return message.principalSource == PrincipalSource::web || message.principalSource == PrincipalSource::trusted;
}

void Store::Impl::initializeConversationArtifactsSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS conversation_artifacts(
  id TEXT PRIMARY KEY NOT NULL, message_id INTEGER NOT NULL REFERENCES conversation_turns(message_id),
  html TEXT NOT NULL)
)SQL");
}

std::string Store::Impl::conversationArtifactImage(const conversation::Message& message, int step,
                                                  std::string_view attachmentId) const {
    const Json args{{"messageId",message.id},{"attachmentId",attachmentId}};
    auto inspected = query(R"SQL(
SELECT 1 FROM conversation_steps s JOIN json_each(s.decision) AS call
WHERE s.message_id=?1 AND s.step<?2 AND s.result IS NOT NULL
  AND json_extract(call.value,'$.tool')='inspect_attachment'
  AND json_type(call.value,'$.rejection') IS NULL
  AND json_extract(call.value,'$.args.messageId')=?1
  AND json_extract(call.value,'$.args.attachmentId')=?3
  AND json_type(s.result,'$[' || call.key || '].ok')='true'
  AND json_extract(s.result,'$[' || call.key || '].data.kind')='image'
LIMIT 1
)SQL",{message.id,step,attachmentId});
    if (inspected.next() && !modelArgumentsContainProtected(redactor(message.bot),"inspect_attachment",args)) {
        auto row = scopedAttachment(message.bot,message.id,args);
        if (row.next()) {
            const auto attachment = row.json(1);
            return "data:" + field(attachment,"mimeType") + ";base64," + field(attachment,"dataBase64");
        }
    }
    throw ToolError("Inspect the referenced image on the current message before publishing it");
}

std::string Store::Impl::embedConversationArtifactImages(const conversation::Message& message, int step,
                                                       std::string_view html) {
    auto marker = html.find(imageMarker);
    if (marker == html.npos) return std::string(html);
    if (Policy(*this).forbidden(message.bot,Action::attachment_inspect,message.bot))
        throw ToolError("Bot attachment access denied");

    std::map<std::string_view,std::string> images;
    std::string expanded;
    while (marker != html.npos) {
        const auto start = marker + imageMarker.size();
        const auto end = html.find("}}",start);
        if (end == html.npos) throw ToolError("Unclosed diagram image reference");
        const auto attachmentId = html.substr(start,end-start);
        auto [image,inserted] = images.try_emplace(attachmentId);
        if (inserted) image->second = conversationArtifactImage(message,step,attachmentId);
        expanded.append(html.substr(0,marker));
        expanded += image->second;
        if (expanded.size() > artifactSize) throw ToolError("HTML exceeds the diagram size limit after embedding images");
        html.remove_prefix(end+2);
        marker = html.find(imageMarker);
    }
    expanded.append(html);
    if (expanded.size() > artifactSize) throw ToolError("HTML exceeds the diagram size limit after embedding images");
    return expanded;
}

Json Store::Impl::publishConversationArtifact(const conversation::Message& message, int step, const Json& args) {
    const auto title = text(args,"title",255,true);
    const auto html = text(args,"html",artifactSize,true);
    if (title.find('\0') != title.npos || html.find('\0') != html.npos)
        throw ToolError("Artifact title and HTML must not contain NUL bytes");
    const auto expanded = embedConversationArtifactImages(message,step,html);
    redactor(message.bot).requirePublic({title,expanded});
    const auto id = actionId("web-");
    exec("INSERT INTO conversation_artifacts(id,message_id,html) VALUES(?,?,?)",{id,message.id,expanded});
    return {{"ok",true},{"artifact",{{"id",id},{"title",title},{"kind",diagramKind}}}};
}

void Store::Impl::classifyLegacyConversationArtifacts(Id reply, Json& artifacts) const {
    if (!artifacts.is_array()) return;
    for (auto& artifact : artifacts) {
        if (!artifact.is_object() || artifact.contains("kind")) continue;
        const auto id = field(artifact,"id");
        if (!id.empty() && query(R"SQL(
SELECT 1 FROM conversation_artifacts a JOIN conversation_turns t ON t.message_id=a.message_id
WHERE a.id=? AND t.reply_id=?
)SQL",{id,reply}).next()) artifact["kind"] = diagramKind;
    }
}

std::optional<std::string> Store::conversationArtifact(const Person& actor, Id reply, std::string_view id) const {
    std::lock_guard lock(p_->mutex);
    auto row = p_->query(R"SQL(
SELECT j.conversation_bot_id,a.html
FROM conversation_artifacts a JOIN conversation_turns t ON t.message_id=a.message_id
JOIN journal j ON j.id=t.reply_id
WHERE a.id=? AND j.id=? AND j.kind='conversation_reply'
  AND EXISTS (SELECT 1 FROM json_each(j.data,'$.artifacts') WHERE json_extract(value,'$.id')=a.id)
)SQL",{id,reply});
    if (!row.next()) return std::nullopt;
    const auto bot = row.string(0);
    if (!Policy(*p_).allows(actor,Action::bot_view,bot)) return std::nullopt;
    auto html = row.string(1);
    if (p_->redactor(bot).containsProtected(html)) return std::nullopt;
    return html;
}
}
