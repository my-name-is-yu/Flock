#include "flock/root_delivery.hpp"
#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace flockv0 {
namespace {
using namespace t3detail;

constexpr std::string_view route = "/api/orchestration/exact-run";
constexpr std::string_view bearerAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr long exactSendTimeoutMs = 35000;
constexpr std::size_t maxExactResponseBytes = 256 * 1024;
constexpr std::size_t maxContextItems = 3;
constexpr std::size_t maxContextBytes = 8192;
constexpr std::size_t maxProgressEvents = 20;
constexpr std::size_t maxProgressSummaryBytes = 120;
constexpr std::array<std::string_view,12> progressStatuses{"idle","pending","preparing","queued","starting","running",
    "waiting","completed","interrupted","failed","cancelled","rolled_back"};

bool identifier(std::string_view value, std::size_t limit = 1024) {
    return !value.empty() && value.size() <= limit && value.front() != ' ' && value.back() != ' ' &&
           std::none_of(value.begin(),value.end(),[](unsigned char character) { return character < 32 || character == 127; });
}

bool valid(const ExactRunTarget& target) {
    return identifier(target.environmentId) && identifier(target.projectId) && identifier(target.threadId) &&
           identifier(target.runId);
}

bool valid(const RootGrantRef& grant) {
    return valid(grant.target) && identifier(grant.grantId,128) && grant.epoch >= 0 && grant.epoch <= maxHistoryPosition;
}

bool valid(const RootCommandRef& ref) { return valid(ref.grant) && identifier(ref.commandId); }
bool valid(const RootInvitationRef& ref) { return valid(ref.target) && identifier(ref.requestId,128); }

bool validCredential(std::string_view credential) {
    return credential.size() == 43 && credential.find_first_not_of(bearerAlphabet) == credential.npos;
}

std::string requiredString(const Json& data, const char* field, std::size_t limit = 1024) {
    auto value = str(data,field);
    if (!identifier(value,limit)) invalidResponse();
    return value;
}

Json targetRequest(const ExactRunTarget& target) {
    return {{"environmentId",target.environmentId},{"projectId",target.projectId},
            {"threadId",target.threadId},{"runId",target.runId}};
}

Json grantRequest(const RootGrantRef& ref) {
    return {{"target",targetRequest(ref.target)},{"grantId",ref.grantId},{"epoch",ref.epoch}};
}

Json commandRequest(const RootCommandRef& ref) {
    auto request = grantRequest(ref.grant);
    request["commandId"] = ref.commandId;
    return request;
}

Json invitationRequest(const RootInvitationRef& ref) {
    return {{"target",targetRequest(ref.target)},{"requestId",ref.requestId}};
}

ExactRunTarget readTarget(const Json& data) {
    const auto& target = member(data,"target");
    return {requiredString(target,"environmentId"),requiredString(target,"projectId"),
            requiredString(target,"threadId"),requiredString(target,"runId")};
}

RootGrantRef grantRef(const Json& data) {
    return {readTarget(data),requiredString(data,"grantId",128),position(member(data,"epoch"))};
}

std::string botId(const Json& data) {
    auto bot = str(data,"botId");
    if (!validBotId(bot)) invalidResponse();
    return bot;
}

std::optional<CoordinationRequest> readPermission(const Json& data) {
    const auto& permission = member(data,"permission");
    const auto kind = str(permission,"kind");
    if (kind == "consult") return {};
    if (kind != "coordinate") invalidResponse();
    const auto expectedEpoch = position(member(permission,"expectedEpoch"));
    auto goal = str(permission,"goal");
    if (expectedEpoch == maxHistoryPosition || goal.empty() || goal.size() > 8192 ||
        trimSessionQuestionText(goal) != goal) invalidResponse();
    return CoordinationRequest{expectedEpoch,std::move(goal),boolean(permission,"allowDelegation")};
}

RootGrant readGrantResponse(const Json& data) {
    auto ref = grantRef(data);
    auto coordination = readPermission(data);
    if (ref.epoch != (coordination ? coordination->expectedEpoch + 1 : 0)) invalidResponse();
    return {std::move(ref),requiredString(data,"sourceMessageId"),botId(data),
            std::move(coordination),boolean(data,"sealed")};
}

RootSealStatus readSealResponse(const Json& data, const RootGrantRef& expected) {
    auto grant = readGrantResponse(member(data,"grant"));
    if (grant.ref != expected) invalidResponse();
    const auto& marker = member(data,"sealMarker");
    const auto& residual = array(data,"residualCommandIds");
    const bool released = boolean(data,"releaseAcknowledged");
    RootSealStatus result{std::move(grant),{},{},released};
    if (!result.grant.sealed) {
        if (!marker.is_null() || !residual.empty() || released) invalidResponse();
        return result;
    }
    result.sealMarker = requiredString(data,"sealMarker");
    std::set<std::string_view> observed;
    for (const auto& item : residual) {
        if (!item.is_string()) invalidResponse();
        const auto& id = item.get_ref<const std::string&>();
        if (!identifier(id) || !observed.insert(id).second) invalidResponse();
        result.residualCommandIds.push_back(id);
    }
    return result;
}

RootInvitation readInvitationResponse(const Json& data, const RootInvitationRef& expected) {
    auto target = readTarget(data);
    auto requestId = requiredString(data,"requestId",128);
    if (target != expected.target || requestId != expected.requestId) invalidResponse();
    RootInvitation invitation{{std::move(target),std::move(requestId),requiredString(data,"sourceMessageId"),botId(data)},
                              requiredString(data,"personRef"),{},readPermission(data)};
    const auto& context = array(data,"context");
    if (context.size() > maxContextItems) invalidResponse();
    for (const auto& item : context) {
        auto itemTarget = readTarget(item);
        auto itemId = requiredString(item,"itemId");
        const auto& text = member(item,"text");
        if (itemTarget != expected.target || !text.is_string() || text.get_ref<const std::string&>().size() > maxContextBytes ||
            std::ranges::any_of(invitation.context,[&](const auto& previous) { return previous.itemId == itemId; }))
            invalidResponse();
        invitation.context.push_back({std::move(itemTarget),std::move(itemId),text.get<std::string>(),boolean(item,"truncated")});
    }
    return invitation;
}

RootProgressPage readProgressResponse(const Json& data, const ExactRunTarget& expected, Id after,
                                      std::string_view credential) {
    auto target = readTarget(data);
    const auto cursor = position(member(data,"cursor"));
    const auto& events = array(data,"events");
    if (target != expected || cursor < after || events.size() > maxProgressEvents) invalidResponse();
    RootProgressPage page{std::move(target),cursor,{}};
    std::set<std::string> seen;
    auto last = after;
    for (const auto& item : events) {
        RootProgressEvent event{requiredString(item,"id"),position(member(item,"position")),
                                requiredString(item,"kind",64),requiredString(item,"status",128),{}};
        const auto& summary = member(item,"summary");
        if (!summary.is_string() || event.position <= last || event.position > cursor || !seen.insert(event.id).second)
            invalidResponse();
        const bool decision = event.kind == "decisionNeeded" && event.status == "pending";
        const bool progress = event.kind == "progress" &&
            std::ranges::find(progressStatuses,event.status) != progressStatuses.end();
        if (!decision && !progress) invalidResponse();
        event.summary = summary.get<std::string>();
        if (event.summary.size() > maxProgressSummaryBytes ||
            event.id.find(credential) != event.id.npos || event.kind.find(credential) != event.kind.npos ||
            event.status.find(credential) != event.status.npos || event.summary.find(credential) != event.summary.npos)
            invalidResponse();
        last = event.position;
        page.events.push_back(std::move(event));
    }
    if (cursor != last) invalidResponse();
    return page;
}

RootReceipt readReceiptResponse(const Json& data, const RootCommandRef& expected) {
    RootCommandRef ref{grantRef(data),requiredString(data,"commandId")};
    if (ref != expected) invalidResponse();
    const auto& observation = member(data,"observation");
    const auto state = str(observation,"state");
    RootReceipt receipt{std::move(ref),botId(data),RootDeliveryState::Unknown,{},{}};
    if (state == "applied") {
        receipt.nativeTurnId = requiredString(observation,"nativeTurnId");
        receipt.nativeItemId = requiredString(observation,"nativeItemId");
        receipt.observation = RootDeliveryState::Applied;
    } else if (state == "pending") receipt.observation = RootDeliveryState::ProviderPending;
    else if (state == "failed") receipt.observation = RootDeliveryState::Failed;
    else if (state != "unknown") invalidResponse();
    return receipt;
}

Json readJson(const HttpResponse& response) {
    const auto type = response.headers.find("content-type");
    if (type == response.headers.end() || response.body.size() > maxExactResponseBytes) invalidResponse();
    auto mediaType = trim(std::string_view(type->second).substr(0,type->second.find(';')));
    std::transform(mediaType.begin(),mediaType.end(),mediaType.begin(),[](unsigned char c) { return std::tolower(c); });
    if (mediaType != "application/json") invalidResponse();
    auto data = parse(response.body);
    if (!data.is_object()) invalidResponse();
    return data;
}

RootGateReply unknown(std::string reason = "unavailable") {
    return {RootGateReply::Kind::Unknown,{},std::move(reason)};
}

RootGateReply found(RootReceipt receipt) {
    return {RootGateReply::Kind::Found,std::move(receipt),{}};
}

std::string refusal(const HttpResponse& response) {
    const auto reason = str(readJson(response),"reason");
    const auto code = response.status;
    if ((code == 400 && reason == "invalidInput") || (code == 401 && reason == "invalidCredential") ||
        (code == 403 && reason == "denied") || (code == 404 && reason == "notFound") ||
        (code == 409 && (reason == "stale" || reason == "conflict")) ||
        (code == 413 && reason == "payloadTooLarge") || (code == 415 && reason == "unsupportedContentType") ||
        (code == 422 && reason == "unsupported")) return reason;
    return {};
}

class ExactRunHttp final : public RootGate {
public:
    explicit ExactRunHttp(T3RootOriginResolver resolver) : resolver_(std::move(resolver)) {}

    std::optional<RootGateCapabilities> capabilities(std::string_view environment) override {
        try {
            const auto origin = resolve(environment);
            if (origin) return probe(*origin,environment);
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<RootGrant> readGrant(const RootGrantRef& ref, std::string_view credential) override {
        if (!valid(ref) || !validCredential(credential)) return {};
        try {
            const auto response = request(ref.target.environmentId,"/grants/read",credential,grantRequest(ref));
            if (response && response->status == 200) {
                auto grant = readGrantResponse(readJson(*response));
                if (grant.ref == ref) return grant;
            }
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<RootSealStatus> sealStatus(const RootGrantRef& ref, std::string_view credential) override {
        if (!valid(ref) || !validCredential(credential)) return {};
        try {
            const auto response = request(ref.target.environmentId,"/grants/seal-status",credential,grantRequest(ref));
            if (response && response->status == 200) return readSealResponse(readJson(*response),ref);
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<RootSealStatus> confirmFreeze(const RootGrantRef& ref, std::string_view marker, Id generation,
                                               std::string_view credential) override {
        if (!valid(ref) || !validCredential(credential) || !identifier(marker) ||
            generation <= 0 || generation > maxHistoryPosition) return {};
        try {
            auto body = grantRequest(ref);
            body["sealMarker"] = marker;
            body["freezeGeneration"] = generation;
            const auto response = request(ref.target.environmentId,"/grants/release",credential,body);
            if (response && response->status == 200) {
                auto acknowledgment = readSealResponse(readJson(*response),ref);
                if (acknowledgment.releaseAcknowledged && acknowledgment.grant.coordination &&
                    acknowledgment.sealMarker && *acknowledgment.sealMarker == marker) return acknowledgment;
            }
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<RootProgressPage> events(const RootGrantRef& ref, Id after, std::string_view credential) override {
        if (!valid(ref) || ref.epoch == 0 || after < 0 || after > maxHistoryPosition || !validCredential(credential)) return {};
        try {
            auto body = grantRequest(ref);
            body["after"] = after;
            const auto response = request(ref.target.environmentId,"/grants/events",credential,body);
            if (response && response->status == 200) return readProgressResponse(readJson(*response),ref.target,after,credential);
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<std::string> inputCommit(const RootGrantRef& ref, std::string_view credential) override {
        if (!valid(ref) || ref.epoch == 0 || !validCredential(credential)) return {};
        try {
            const auto response = request(ref.target.environmentId,"/grants/input-commit",credential,grantRequest(ref),
                                          6000,true);
            if (!response || response->status != 200) return {};
            const auto data = readJson(*response);
            if (readTarget(data) != ref.target) return {};
            auto commit = str(data,"commit");
            if ((commit.size() == 40 || commit.size() == 64) &&
                commit.find_first_not_of("0123456789abcdef") == commit.npos && commit.find(credential) == commit.npos)
                return commit;
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<RootInvitation> readInvitation(const RootInvitationRef& ref, std::string_view credential) override {
        if (!valid(ref) || !validCredential(credential)) return {};
        try {
            const auto response = request(ref.target.environmentId,"/invitations/read",credential,invitationRequest(ref));
            if (response && response->status == 200) return readInvitationResponse(readJson(*response),ref);
        } catch (const std::exception&) {}
        return {};
    }

    std::optional<RootGrant> activateInvitation(const RootInvitationRef& ref, std::string_view activationId,
                                                std::string_view credential) override {
        if (!valid(ref) || !identifier(activationId,128) || !validCredential(credential)) return {};
        try {
            auto body = invitationRequest(ref);
            body["activationId"] = activationId;
            const auto response = request(ref.target.environmentId,"/invitations/activate",credential,body);
            if (response && response->status == 200) {
                auto grant = readGrantResponse(readJson(*response));
                const auto expectedId = grant.coordination ? activationId : std::string_view(ref.requestId);
                if (grant.ref.target == ref.target && grant.ref.grantId == expectedId) return grant;
            }
        } catch (const std::exception&) {}
        return {};
    }

    RootGateReply readReceipt(const RootCommandRef& ref, std::string_view credential) override {
        if (!valid(ref) || !validCredential(credential)) return unknown();
        try {
            const auto response = request(ref.grant.target.environmentId,"/commands/receipt",credential,commandRequest(ref));
            if (!response || response->status != 200) return unknown();
            const auto data = readJson(*response);
            const auto status = str(data,"status");
            if (status == "found") return found(readReceiptResponse(member(data,"receipt"),ref));
            if (status == "authoritativelyAbsent" && !data.contains("receipt"))
                return {RootGateReply::Kind::AuthoritativelyAbsent,{},{}};
        } catch (const std::exception&) {}
        return unknown();
    }

    RootGateReply send(const RootCommand& command, std::string_view credential) override {
        if (!valid(command.ref) || !validCredential(credential) || command.text.empty() ||
            command.text.size() > maxSessionPromptBytes)
            return {RootGateReply::Kind::Rejected,{},"invalidInput"};
        try {
            auto body = commandRequest(command.ref);
            body["text"] = command.text;
            const auto response = request(command.ref.grant.target.environmentId,"/commands/send",credential,body,exactSendTimeoutMs);
            if (!response) return unknown();
            if (response->status == 200) return found(readReceiptResponse(readJson(*response),command.ref));
            if (response->status >= 400 && response->status < 500) {
                auto reason = refusal(*response);
                if (!reason.empty()) return {RootGateReply::Kind::Rejected,{},std::move(reason)};
            }
        } catch (const std::exception&) {}
        return unknown();
    }

private:
    std::optional<std::string> resolve(std::string_view environment) const {
        if (!resolver_ || !identifier(environment)) return {};
        const auto configured = resolver_(environment);
        if (!configured || configured->find('\0') != configured->npos) return {};
        const auto url = checkedUrl(*configured);
        if (url.path != "/" && url.path != "/mcp") return {};
        return url.origin;
    }

    std::optional<RootGateCapabilities> probe(const std::string& origin, std::string_view environment) const {
        const auto response = http(origin + std::string(route) + "/capabilities","GET",{{"Accept","application/json"}});
        if (response.status != 200) return {};
        const auto data = readJson(response);
        RootGateCapabilities result{requiredString(data,"environmentId"),requiredString(data,"serverVersion",256),
                                    position(member(data,"protocolVersion")),boolean(data,"exactTarget"),
                                    boolean(data,"grantSeal"),boolean(data,"receiptLookup"),boolean(data,"senderAttribution"),
                                    data.contains("strictPinnedWorktree") && boolean(data,"strictPinnedWorktree")};
        if (result.environmentId != environment || result.protocolVersion != 1 || !result.exactTarget ||
            !result.grantSeal || !result.receiptLookup || !result.senderAttribution) return {};
        return result;
    }

    std::optional<HttpResponse> request(std::string_view environment, std::string_view path,
                                        std::string_view credential, const Json& body, long timeoutMs = 6000,
                                        bool requiresPinnedWorktree = false) const {
        const auto origin = resolve(environment);
        if (!origin) return {};
        const auto capabilities = probe(*origin,environment);
        if (!capabilities || (requiresPinnedWorktree && !capabilities->strictPinnedWorktree)) return {};
        return http(*origin + std::string(route) + std::string(path),"POST",
                    {{"Accept","application/json"},{"Content-Type","application/json"},
                     {"Authorization","Bearer " + std::string(credential)}},body.dump(),timeoutMs);
    }

    T3RootOriginResolver resolver_;
};
}

std::unique_ptr<RootGate> makeT3RootGate(T3RootOriginResolver resolver) {
    return std::make_unique<ExactRunHttp>(std::move(resolver));
}

std::unique_ptr<RootGate> makeT3RootGate(const std::filesystem::path& credentialDirectory) {
    return makeT3RootGate([credentialDirectory](std::string_view environment) -> std::optional<std::string> {
        return loadCredential(credentialDirectory,std::string(environment)).mcpUrl;
    });
}
}
