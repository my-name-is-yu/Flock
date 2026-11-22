#include "session_handoff_routes.hpp"
#include "access_routes.hpp"

#include <algorithm>
#include <charconv>

namespace flockv0::web {
namespace {
constexpr std::size_t maxPayload = 16 * 1024;

struct InvalidHandoffInput {};

void reply(httplib::Response& res, int status, const Json& data) {
    res.status = status;
    res.set_content(data.dump(),"application/json; charset=utf-8");
}

std::string identifier(const Json& object, std::string_view field, std::size_t maxBytes = 1024) {
    const auto value = object.find(field);
    if (value == object.end() || !value->is_string()) throw InvalidHandoffInput{};
    const auto& text = value->get_ref<const std::string&>();
    if (text.empty() || text.size() > maxBytes || text.front() == ' ' || text.back() == ' ' ||
        std::ranges::any_of(text,[](unsigned char c) { return c < 32 || c == 127; })) throw InvalidHandoffInput{};
    return text;
}

Json objectBody(const httplib::Request& req, std::size_t count) {
    auto body = Json::parse(req.body,nullptr,false);
    if (!body.is_object() || body.size() != count) throw InvalidHandoffInput{};
    return body;
}

Id channelId(const httplib::Request& req) {
    const auto text = req.matches[1].str();
    Id id = 0;
    const auto parsed = std::from_chars(text.data(),text.data() + text.size(),id);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || id <= 0) throw InvalidHandoffInput{};
    return id;
}

Json targetView(const ExactRunTarget& target) {
    return {{"environmentId",target.environmentId},{"projectId",target.projectId},
            {"threadId",target.threadId},{"runId",target.runId}};
}

Json deliveryView(const RootDeliveryView& delivery) {
    Json view{{"commandId",delivery.ref.commandId},{"state",rootDeliveryStateName(delivery.state)},
              {"grantEpoch",delivery.ref.grant.epoch},{"sealed",delivery.sealed},{"provider",nullptr}};
    if (const auto& receipt = delivery.receipt) {
        view["provider"] = {{"state",rootDeliveryStateName(receipt->observation)}};
        if (receipt->observation == RootDeliveryState::Applied) {
            view["provider"]["nativeTurnId"] = receipt->nativeTurnId;
            view["provider"]["nativeItemId"] = receipt->nativeItemId;
        }
    }
    return view;
}

Json handoffView(const SessionHandoffView& handoff) {
    const auto& invitation = handoff.origin.invitation;
    return {{"channelId",handoff.channel},{"target",targetView(invitation.target)},{"requestId",invitation.requestId},
            {"sourceMessageId",invitation.sourceMessageId},{"botId",invitation.botId},
            {"mode",handoff.coordination ? "coordinate" : "consult"},{"opening",handoff.opening},
            {"person",handoff.origin.personId},{"grantEpoch",handoff.epoch ? Json(*handoff.epoch) : Json(nullptr)},
            {"goal",handoff.permission ? Json(handoff.permission->goal) : Json(nullptr)},
            {"allowDelegation",handoff.permission && handoff.permission->allowDelegation},
            {"sealed",handoff.sealed},{"frozen",handoff.frozen},{"releaseAcknowledged",handoff.releaseAcknowledged},
            {"released",handoff.sealed && handoff.frozen && (!handoff.coordination || handoff.releaseAcknowledged)},
            {"generation",handoff.generation},{"residualCommandIds",handoff.residualCommandIds},
            {"sealMarker",handoff.sealMarker ? Json(*handoff.sealMarker) : Json(nullptr)},
            {"reply",handoff.reply ? deliveryView(*handoff.reply) : Json(nullptr)},
            {"eventCursor",handoff.eventCursor},{"latestDecision",handoff.latestDecision},
            {"pendingQuestion",handoff.pendingQuestion},{"work",handoff.work},
            {"admittedChildIds",handoff.admittedChildIds}};
}

template<class Action>
void withHandoff(const WebCallbacks& callbacks, const httplib::Request& req, httplib::Response& res, Action action) {
    if (!callbacks.authenticate) return reply(res,401,{{"error","identity_required"}});
    const auto actor = person(callbacks,req);
    if (actor.id.empty()) return reply(res,401,{{"error","identity_required"}});
    if (!callbacks.sessionHandoffs) return reply(res,503,{{"error","handoff_unavailable"}});
    if (req.body.size() > maxPayload) return reply(res,413,{{"error","payload_too_large"}});
    if (!req.params.empty()) return reply(res,400,{{"error","invalid_input"}});
    try {
        action(*callbacks.sessionHandoffs,actor);
    } catch (const InvalidHandoffInput&) {
        reply(res,400,{{"error","invalid_input"}});
    } catch (const Conflict&) {
        reply(res,403,{{"error","handoff_unavailable"}});
    } catch (const std::exception&) {
        reply(res,503,{{"error","handoff_unavailable"}});
    }
}
}

void sessionHandoffRoutes(httplib::Server& server, const WebCallbacks& callbacks) {
    server.Post("/api/session-handoffs",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            const auto body = objectBody(req,3);
            const auto target = body.find("target");
            if (target == body.end() || !target->is_object() || target->size() != 4) throw InvalidHandoffInput{};
            RootInvitationRef invitation{{identifier(*target,"environmentId"),identifier(*target,"projectId"),
                                          identifier(*target,"threadId"),identifier(*target,"runId")},
                                         identifier(body,"requestId",128)};
            const auto credential = identifier(body,"credential",43);
            if (credential.size() != 43 ||
                credential.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != credential.npos)
                throw InvalidHandoffInput{};
            reply(res,200,handoffView(handoff.accept(actor,invitation,credential)));
        });
    });
    server.Get(R"(/api/session-handoffs/([0-9]+))",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            reply(res,200,handoffView(handoff.view(actor,channelId(req))));
        });
    });
    server.Post(R"(/api/session-handoffs/([0-9]+)/answer)",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            const auto body = objectBody(req,3);
            const auto generation = body.find("generation");
            const auto text = body.find("text");
            if (generation == body.end() || !generation->is_number_integer() || generation->get<Id>() <= 0 ||
                text == body.end() || !text->is_string() || text->get_ref<const std::string&>().size() > 8192)
                throw InvalidHandoffInput{};
            reply(res,200,handoffView(handoff.answer(actor,channelId(req),generation->get<Id>(),
                                                     identifier(body,"eventId"),text->get_ref<const std::string&>())));
        });
    });
    server.Post(R"(/api/session-handoffs/([0-9]+)/work-answer)",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            const auto body = objectBody(req,5);
            const auto generation = body.find("generation");
            const auto wait = body.find("waitId");
            const auto text = body.find("text");
            const auto stage = body.find("stageId");
            if (generation == body.end() || !generation->is_number_integer() || generation->get<Id>() <= 0 ||
                wait == body.end() || !wait->is_number_integer() || wait->get<Id>() <= 0 ||
                text == body.end() || !text->is_string() || text->get_ref<const std::string&>().size() > 8000 ||
                stage == body.end() || (!stage->is_null() && !stage->is_string())) throw InvalidHandoffInput{};
            const auto stageId = stage->is_null() ? std::string() : identifier(body,"stageId",80);
            reply(res,200,handoffView(handoff.answerWork(actor,channelId(req),generation->get<Id>(),
                identifier(body,"delegationId",128),stageId,wait->get<Id>(),text->get_ref<const std::string&>())));
        });
    });
    server.Post(R"(/api/session-handoffs/([0-9]+)/(resume|consult|freeze|step))",[&callbacks](const httplib::Request& req, httplib::Response& res) {
        withHandoff(callbacks,req,res,[&](SessionHandoff& handoff, const Person& actor) {
            objectBody(req,0);
            const auto channel = channelId(req);
            if (req.matches[2] == "resume") return reply(res,200,handoffView(handoff.resume(actor,channel)));
            if (req.matches[2] == "freeze") return reply(res,200,handoffView(handoff.freeze(actor,channel)));
            if (!callbacks.sessionHandoffModel || !callbacks.sessionHandoffSoul)
                return reply(res,503,{{"error","handoff_unavailable"}});
            if (req.matches[2] == "step")
                return reply(res,200,handoffView(handoff.step(actor,channel,*callbacks.sessionHandoffModel,
                                                               callbacks.intake,callbacks.sessionHandoffSoul)));
            const auto delivery = handoff.consult(actor,channel,*callbacks.sessionHandoffModel,callbacks.intake,callbacks.sessionHandoffSoul);
            reply(res,200,deliveryView(delivery));
        });
    });
}
}
