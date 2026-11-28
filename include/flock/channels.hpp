#pragma once
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0 {
enum class ChannelKind { slack, imessage };
std::string_view channelName(ChannelKind);

struct ChannelCapabilities {
    bool supportsReplies, supportsEdit, supportsReact, supportsHtmlArtifacts;
    std::vector<std::string> allowedAttachmentTypes;
    std::size_t maxAttachmentBytes;
};

struct ChannelOrigin {
    ChannelKind kind;
    std::string workspaceId, threadRef, externalMessageId;
    Json json() const;
    std::string key() const;
};

struct ChannelSender {
    std::string botId, personId;
};

struct ChannelDelivery {
    enum class State { accepted, unknown, rejected, notSent } state = State::unknown;
    std::string externalMessageId;
};

class ChannelTransport {
public:
    virtual ~ChannelTransport() = default;
    virtual ChannelDelivery send(std::string_view key, const ChannelOrigin&, std::string_view operation, const Json& payload) = 0;
    virtual ChannelDelivery reconcile(std::string_view key, const ChannelOrigin&) = 0;
};

struct ChannelConnector {
    ChannelKind kind;
    ChannelCapabilities capabilities;
    std::function<bool(std::string_view credential, std::string_view workspace)> authenticates;
    std::function<std::optional<ChannelSender>(std::string_view workspace, std::string_view sender)> resolveSender;
    std::shared_ptr<ChannelTransport> transport;
};

ChannelCapabilities channelCapabilities(ChannelKind);
std::string channelPolicyTarget(const ChannelOrigin&);
std::string normalizeArtifactBaseUrl(std::string);

struct ChannelResource {
    std::string name, mimeType, bytes;
};

struct VerifiedChannelMessage {
    ChannelOrigin origin;
    std::string senderId, personId;
    std::vector<ChannelResource> attachments;
};

struct ChannelAccessDenied : Conflict {
    using Conflict::Conflict;
};

class LocalMachine {
public:
    virtual ~LocalMachine() = default;
    virtual std::string_view owner() const = 0;
    virtual Json run(std::string_view operation, const std::filesystem::path& path) = 0;
};
std::shared_ptr<LocalMachine> hostLocalMachine(std::string owner);

std::string encodeAttachment(std::string_view);
std::string decodeAttachment(std::string_view, std::size_t maxBytes);

class Store;
class ChannelStorage;
class ConversationTurns;
class Database;
struct Person;

class Channels {
public:
    explicit Channels(Store&);

    void connectChannel(ChannelConnector);
    void artifactBaseUrl(std::string url);
    Json receiveChannel(ChannelKind, std::string_view credential, const Json& event, const IntakeResolver& routing);
    std::optional<ChannelResource> artifact(std::string_view id) const;
    std::optional<ChannelResource> attachment(const Person&, Id id) const;
private:
    Database& database_;
    ChannelStorage& channels_;
    ConversationTurns& turns_;
};

}
