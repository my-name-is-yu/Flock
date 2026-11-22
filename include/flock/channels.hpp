#pragma once
#include "flock/access.hpp"

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
    virtual ChannelDelivery send(std::string_view key, const ChannelOrigin&, std::string_view operation,
                                 const Json& payload) = 0;
    virtual ChannelDelivery reconcile(std::string_view key, const ChannelOrigin&) = 0;
};

using AuthenticateChannel = std::function<std::optional<ChannelSender>(
    std::string_view credential, std::string_view workspace, std::string_view sender)>;

class ChannelConnector {
public:
    virtual ~ChannelConnector() = default;
    ChannelKind kind() const { return kind_; }
    const ChannelCapabilities& capabilities() const { return capabilities_; }
    std::optional<ChannelSender> authenticate(std::string_view, std::string_view, std::string_view) const;
    ChannelDelivery send(std::string_view key, const ChannelOrigin&, std::string_view operation,
                         const Json& payload) const;
    ChannelDelivery reconcile(std::string_view key, const ChannelOrigin&) const;
protected:
    ChannelConnector(ChannelKind, ChannelCapabilities, AuthenticateChannel, std::shared_ptr<ChannelTransport>);
private:
    ChannelKind kind_;
    ChannelCapabilities capabilities_;
    AuthenticateChannel authenticate_;
    std::shared_ptr<ChannelTransport> transport_;
};

class SlackConnector final : public ChannelConnector {
public:
    SlackConnector(AuthenticateChannel, std::shared_ptr<ChannelTransport>,
                   ChannelCapabilities = {true,true,true,true,{"image/png","image/jpeg","image/webp","text/plain"},4*1024*1024});
};
class IMessageConnector final : public ChannelConnector {
public:
    IMessageConnector(AuthenticateChannel, std::shared_ptr<ChannelTransport>,
                      ChannelCapabilities = {true,false,true,true,{"image/png","image/jpeg","image/webp","text/plain"},4*1024*1024});
};

Scope channelScope(const ChannelOrigin&, std::string_view operation);

struct ChannelAttachment {
    std::string name, mimeType, bytes;
};

struct VerifiedChannelMessage {
    ChannelOrigin origin;
    std::string personId;
    std::vector<ChannelAttachment> attachments;
};

struct ChannelResource {
    std::string name, mimeType, bytes;
};

struct ChannelAccessDenied : Conflict { using Conflict::Conflict; };

class LocalMachine {
public:
    virtual ~LocalMachine() = default;
    virtual std::string_view owner() const = 0;
    virtual Json run(std::string_view operation, const std::filesystem::path& path) = 0;
};
std::shared_ptr<LocalMachine> hostLocalMachine(std::string owner);

std::string encodeAttachment(std::string_view);
std::string decodeAttachment(std::string_view, std::size_t maxBytes);
}
