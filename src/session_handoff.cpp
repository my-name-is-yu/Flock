#include "flock/session_handoff.hpp"
#include "storage.hpp"

#include <algorithm>
#include <tuple>

namespace flockv0 {
namespace {

const ReturnChannel& channelOf(const Participation& participation) {
    return std::visit([](const auto& entry) -> const ReturnChannel& { return entry.channel; }, participation);
}

class InvitationAuthority final : public ParticipationAuthority {
public:
    std::optional<VerifiedInvitation> verified;
    std::optional<CoordinationGrant> acknowledgment;

    std::optional<VerifiedInvitation> invitation(const InvitationRef& ref) override {
        return verified && verified->ref == ref ? verified : std::nullopt;
    }
    std::optional<CoordinationGrant> grant(const CoordinationBinding& binding) override {
        return acknowledgment && acknowledgment->binding == binding ? acknowledgment : std::nullopt;
    }
};

RootInvitationRef lookupRef(const ReturnChannel& channel) {
    return {channel.origin.invitation.target, channel.origin.invitation.requestId};
}

Json workQuestion(const Json& task, const SecretRedactor& scrub) {
    const auto& wait = task.at("wait");
    if (task.at("state") != "waiting" || !wait.is_object() || field(wait,"reason") != "user_question")
        return nullptr;
    return {{"waitId",wait.at("id")},{"text",scrub.text(field(wait,"text"))}};
}

}

void Store::Impl::initializeSessionHandoffSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_handoffs(
 channel_id INTEGER PRIMARY KEY REFERENCES return_channels(id),
 t3_person_ref TEXT NOT NULL,
 event_cursor INTEGER NOT NULL DEFAULT 0)
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS handoff_decisions(
 channel_id INTEGER NOT NULL REFERENCES session_handoffs(channel_id),
 event_id TEXT NOT NULL,
 position INTEGER NOT NULL,
 generation INTEGER NOT NULL,
 command_id TEXT NOT NULL UNIQUE,
 kind TEXT NOT NULL CHECK(kind IN ('root_guidance','handoff_question','wait_for_root','delegate_specialist')),
 text TEXT NOT NULL,
 settled INTEGER NOT NULL DEFAULT 0,
 answer_text TEXT,
 answer_consumed INTEGER NOT NULL DEFAULT 0,
 PRIMARY KEY(channel_id,event_id))
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS handoff_specialist_intents(
 command_id TEXT PRIMARY KEY REFERENCES handoff_decisions(command_id),
 specialist TEXT NOT NULL,
 done_when TEXT NOT NULL,
 commit_sha TEXT NOT NULL)
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS handoff_reviewed_results(
 channel_id INTEGER NOT NULL REFERENCES session_handoffs(channel_id),
 journal_id INTEGER NOT NULL REFERENCES journal(id),
 command_id TEXT PRIMARY KEY REFERENCES handoff_decisions(command_id),
 commit_sha TEXT NOT NULL,
 UNIQUE(channel_id,journal_id))
)SQL");
}

SessionHandoffView SessionHandoff::accept(const Person& person, const RootInvitationRef& ref,
                                          std::string_view credential) {
    if (credential.size() != 43 || credential.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != credential.npos)
        throw Conflict("Invalid Session invitation credential");
    const auto remote = gate_.readInvitation(ref, credential);
    if (!remote || remote->ref.target != ref.target || remote->ref.requestId != ref.requestId ||
        !bindsPerson_ || !bindsPerson_(ref.target.environmentId, remote->personRef, person)) {
        throw Conflict("The T3 invitation could not be verified for this person");
    }
    const SecretRedactor scrub({std::string(credential)});
    scrub.requirePublic({remote->personRef, remote->ref.requestId, remote->ref.sourceMessageId, remote->ref.botId,
                         ref.target.environmentId, ref.target.projectId, ref.target.threadId, ref.target.runId});
    if (remote->coordination) scrub.requirePublic({remote->coordination->goal});
    auto context = remote->context;
    for (auto& item : context) {
        scrub.requirePublic({item.itemId});
        item.text = scrub.text(item.text, item.truncated);
    }
    InvitationAuthority authority;
    authority.verified = VerifiedInvitation{remote->ref, person.id, std::move(context), remote->coordination};
    const auto participation = ParticipationStore(store_).open(person, remote->ref, authority);
    const auto channel = channelOf(participation).id;
    auto& db = *store_.p_;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.authorizeParticipation(person, channel);
        db.sharedRedactor().requirePublic({remote->personRef});
        auto existing = db.query("SELECT t3_person_ref FROM session_handoffs WHERE channel_id=?", {channel});
        if (existing.next()) {
            if (existing.string(0) != remote->personRef ||
                db.secrets.read(participationCredential(channel)) != credential)
                throw Conflict("A different invitation is already recorded");
        } else {
            db.secrets.insert(participationCredential(channel), credential);
            db.exec("INSERT INTO session_handoffs(channel_id,t3_person_ref) VALUES(?,?)",
                    {channel, remote->personRef});
        }
        tx.commit();
    }
    return resume(person, channel);
}

SessionHandoffView SessionHandoff::resume(const Person& person, Id channel) {
    const auto current = view(person, channel);
    if (!current.opening) {
        RootDelivery delivery(store_,gate_);
        delivery.observeGrant(person,channel);
        if (current.reply)
            delivery.reconcile(person,current.origin.invitation.botId,channel,current.reply->ref.commandId);
        return view(person, channel);
    }
    auto& db = *store_.p_;
    const auto [participation, personRef, credential] = [&] {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db, Transaction::Mode::read);
        auto row = db.query("SELECT t3_person_ref FROM session_handoffs WHERE channel_id=?", {channel});
        if (!row.next()) throw Conflict("Unknown Session invitation");
        auto permission = db.authorizeParticipation(person, channel);
        auto secret = db.secrets.read(participationCredential(channel));
        auto ref = row.string(0);
        tx.commit();
        return std::tuple{std::move(permission), std::move(ref), std::move(secret)};
    }();
    const auto& origin = channelOf(participation).origin;
    const auto ref = lookupRef(channelOf(participation));
    if (!bindsPerson_ || !bindsPerson_(ref.target.environmentId, personRef, person))
        throw Conflict("The Session invitation identity is no longer authorized");
    const auto* coordination = std::get_if<CoordinationActivation>(&participation);
    const auto& id = coordination ? coordination->binding.activationId : ref.requestId;
    const auto grant = gate_.activateInvitation(ref, id, credential);
    if (!grant) return view(person, channel);
    const auto expectedEpoch = coordination ? coordination->binding.request.expectedEpoch + 1 : 0;
    const auto permission = coordination ? std::optional(coordination->binding.request) : std::nullopt;
    if (grant->ref != RootGrantRef{ref.target, id, expectedEpoch} ||
        grant->sourceMessageId != origin.invitation.sourceMessageId || grant->botId != origin.invitation.botId ||
        grant->coordination != permission) throw Conflict("T3 returned a different invitation grant");
    if (coordination) {
        InvitationAuthority authority;
        authority.acknowledgment = CoordinationGrant{coordination->binding, grant->ref.epoch};
        ParticipationStore(store_).observeGrant(person, channel, authority);
    }
    RootDelivery(store_, gate_).bind(person, channel, credential);
    return view(person, channel);
}

SessionHandoffView SessionHandoff::freeze(const Person& person, Id channel) {
    view(person, channel);
    RootDelivery(store_, gate_).freeze(person, channel);
    return view(person, channel);
}

SessionHandoffView SessionHandoff::answer(const Person& person, Id channel, Id generation,
                                        std::string_view eventId, std::string_view text) {
    const auto handoff = view(person,channel);
    if (!handoff.coordination || handoff.opening || handoff.sealed || handoff.frozen ||
        handoff.generation != generation || eventId.empty() || eventId.size() > 1024 || text.size() > 8192)
        throw Conflict("This question is unavailable");
    const auto grant = RootDelivery(store_,gate_).observeGrant(person,channel);
    if (!grant.verified || grant.sealed || grant.frozen || grant.generation != generation)
        throw Conflict("This question is unavailable");
    auto& db = *store_.p_;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.authorizeParticipation(person,channel);
        const auto credential = db.secrets.read(participationCredential(channel));
        const auto cleaned = SecretRedactor({credential}).text(db.sharedRedactor().text(text));
        if (trimSessionQuestionText(cleaned).empty()) throw Conflict("The answer is empty");
        auto row = db.query(R"SQL(
SELECT d.answer_text FROM handoff_decisions d JOIN root_gate_channels g ON g.channel_id=d.channel_id
WHERE d.channel_id=? AND d.event_id=? AND d.generation=? AND d.kind='handoff_question'
AND g.generation=d.generation AND NOT g.sealed AND NOT g.frozen
)SQL", {channel,eventId,generation});
        if (!row.next() || (!row.string(0).empty() && row.string(0) != cleaned))
            throw Conflict("This question is unavailable");
        db.exec("UPDATE handoff_decisions SET answer_text=? WHERE channel_id=? AND event_id=? AND answer_text IS NULL",
                {cleaned,channel,eventId});
        tx.commit();
    }
    return view(person,channel);
}

SessionHandoffView SessionHandoff::answerWork(const Person& person, Id channel, Id generation,
                                            std::string_view delegation, std::string_view stage, Id wait,
                                            std::string_view text) {
    const auto handoff = view(person,channel);
    if (!handoff.permission || !handoff.permission->allowDelegation || handoff.opening ||
        handoff.generation != generation || wait <= 0 || text.size() > 8000)
        throw Conflict("This work question is unavailable");
    const auto grant = RootDelivery(store_,gate_).observeGrant(person,channel);
    if (!grant.verified || grant.sealed || grant.frozen || grant.generation != generation)
        throw Conflict("This work question is unavailable");
    auto& db = *store_.p_;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.authorizeParticipation(person,channel);
        auto binding = db.query(R"SQL(
SELECT CASE WHEN ?4='' THEN d.sender_task ELSE s.task_id END
FROM specialist_delegations d JOIN root_gate_channels g ON g.channel_id=d.channel_id
LEFT JOIN workflows w ON w.delegation_id=d.id
LEFT JOIN workflow_stages s ON s.workflow_id=w.id AND s.stage_id=?4
WHERE d.channel_id=?1 AND d.generation=?2 AND d.id=?3 AND d.generation=g.generation
AND NOT g.sealed AND NOT g.frozen
)SQL", {channel,generation,delegation,stage});
        if (!binding.next() || !binding.number(0)) throw Conflict("This work question is unavailable");
        const auto task = db.load(binding.number(0));
        const auto question = workQuestion(task,db.sharedRedactor());
        if (question.is_null() || question.at("waitId") != wait) throw Conflict("This work question is unavailable");
        const auto credential = db.secrets.read(participationCredential(channel));
        const auto cleaned = SecretRedactor({credential}).text(db.sharedRedactor().text(text));
        db.operate(person,TaskTarget::from(task,wait),"answer",{{"text",cleaned}});
        tx.commit();
    }
    return view(person,channel);
}

SessionHandoffView SessionHandoff::view(const Person& person, Id channel) const {
    auto& db = *store_.p_;
    SessionHandoffView result;
    std::string commandId;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db, Transaction::Mode::read);
        const auto participation = db.authorizeParticipation(person, channel);
        const auto& origin = channelOf(participation).origin;
        auto row = db.query(R"SQL(
SELECT h.t3_person_ref,g.sealed,g.frozen,g.grant_id,g.generation,g.residual_commands,g.seal_marker,
       g.release_acknowledged,h.event_cursor FROM session_handoffs h
LEFT JOIN root_gate_channels g ON g.channel_id=h.channel_id WHERE h.channel_id=?
)SQL", {channel});
        if (!row.next() || !bindsPerson_ ||
            !bindsPerson_(origin.invitation.target.environmentId, row.string(0), person))
            throw Conflict("Session invitation is unavailable");
        result.channel = channel;
        result.origin = origin;
        result.coordination = std::holds_alternative<CoordinationActivation>(participation);
        result.opening = row.string(3).empty();
        result.sealed = row.number(1) != 0;
        result.frozen = row.number(2) != 0;
        if (const auto* coordination = std::get_if<CoordinationActivation>(&participation)) {
            result.permission = coordination->binding.request;
            if (coordination->acknowledgment) result.epoch = coordination->acknowledgment->epoch;
        } else if (!result.opening) result.epoch = 0;
        if (!result.opening) {
            result.generation = row.number(4);
            result.residualCommandIds = row.json(5).get<std::vector<std::string>>();
            if (auto marker = row.string(6); !marker.empty()) result.sealMarker = std::move(marker);
            result.releaseAcknowledged = row.number(7) != 0;
        }
        result.eventCursor = row.number(8);
        if (auto decision = db.query(R"SQL(
SELECT event_id,position,command_id,kind,text,settled FROM handoff_decisions
WHERE channel_id=? AND kind<>'wait_for_root' ORDER BY rowid DESC LIMIT 1
)SQL", {channel}); decision.next()) {
            result.latestDecision = {{"eventId",decision.string(0)},{"position",decision.number(1)},
                                     {"kind",decision.string(3)},{"text",decision.string(4)},
                                     {"settled",decision.number(5) != 0}};
            if (decision.string(3) == "root_guidance" &&
                db.query("SELECT 1 FROM root_deliveries WHERE channel_id=? AND command_id=?",
                         {channel, decision.string(2)}).next()) commandId = decision.string(2);
        }
        if (!result.opening && !result.sealed && !result.frozen) {
            auto question = db.query(R"SQL(
SELECT event_id,text FROM handoff_decisions WHERE channel_id=? AND generation=? AND kind='handoff_question'
AND answer_text IS NULL ORDER BY rowid LIMIT 1
)SQL", {channel,result.generation});
            if (question.next()) result.pendingQuestion = {{"eventId",question.string(0)},{"text",question.string(1)}};
        }
        auto delegations = db.query(R"SQL(
SELECT d.id,r.bot_id,r.state,d.purpose,d.done_when,d.commit_sha,w.id,j.data,p.phase,d.sender_task,d.parent_id
FROM specialist_delegations d JOIN tasks r ON r.id=d.recipient_task
JOIN peer_exchanges p ON p.id=d.id LEFT JOIN workflows w ON w.delegation_id=d.id
LEFT JOIN journal j ON j.task_id=d.sender_task AND j.kind='specialist_result'
WHERE d.channel_id=? ORDER BY d.rowid LIMIT 16
)SQL", {channel});
        while (delegations.next()) {
            const auto scrub = db.sharedRedactor();
            Json work{{"delegationId",delegations.string(0)},{"executor",delegations.string(1)},
                      {"state",delegations.string(2)},{"purpose",delegations.string(3)},
                      {"doneWhen",delegations.string(4)},{"commit",delegations.string(5)},
                      {"phase",delegations.string(8)},{"stages",Json::array()},
                      {"question",workQuestion(db.load(delegations.number(9)),scrub)}};
            if (const auto parent = delegations.string(10); !parent.empty()) work["parentDelegationId"] = parent;
            if (const auto workflow = delegations.number(6)) {
                const auto details = db.workflowStatus(workflow,delegations.string(1),unixTime());
                for (const auto& stage : details.at("stages")) {
                    const auto task = stage.at("taskId").get<Id>();
                    work["stages"].push_back({{"stageId",stage.at("stageId")},{"name",stage.at("name")},
                        {"status",stage.at("status")},{"needsAttention",stage.at("needsAttention")},
                        {"commit",stage.at("inputArtifactVersion")},
                        {"question",task ? workQuestion(db.load(task),scrub) : Json(nullptr)}});
                }
            }
            if (!delegations.string(7).empty()) {
                const auto settled = delegations.json(7);
                work["result"] = {{"status",settled.value("workflowStatus",field(settled,"status"))},
                                  {"verdict",field(settled,"verdict")},{"report",field(settled,"report")}};
                auto evidence = Json::array();
                for (const auto& source : settled.value("references",Json::array())) {
                    evidence.push_back({{"threadId",field(source,"threadId")},{"runId",field(source,"runId")},
                                        {"itemId",field(source,"itemId")},{"stageId",field(source,"stageId")},
                                        {"verdict",field(source,"verdict")},{"commit",field(source,"artifactVersion")}});
                }
                work["result"]["evidence"] = std::move(evidence);
            }
            result.work.push_back(std::move(work));
        }
        auto children = db.query(R"SQL(
SELECT l.action_id FROM specialist_launches l
JOIN specialist_delegations d ON d.id=l.delegation_id WHERE d.channel_id=? ORDER BY l.rowid
)SQL", {channel});
        while (children.next()) result.admittedChildIds.push_back(children.string(0));
        if (const auto* consult = std::get_if<ConsultPermit>(&participation);
            consult && db.query("SELECT 1 FROM root_deliveries WHERE channel_id=? AND command_id=?",
                                {channel, consult->commandId}).next())
            commandId = consult->commandId;
        tx.commit();
    }
    if (!commandId.empty())
        result.reply = RootDelivery(store_, gate_).view(person, result.origin.invitation.botId, channel, commandId);
    return result;
}

RootDeliveryView SessionHandoff::consult(const Person& person, Id channel, Model& model,
                                         const IntakeResolver& intake, const Runtime::SoulLoader& soul) {
    const auto status = view(person, channel);
    const auto& bot = status.origin.invitation.botId;
    const auto permit = ParticipationStore(store_).consult(person, bot, channel);
    RootDelivery delivery(store_, gate_);
    if (status.reply) return delivery.deliver(person, bot, channel, permit.commandId);
    if (status.opening || status.sealed || status.frozen) throw Conflict("Consultation grant is not active");
    const auto grant = delivery.observeGrant(person, channel);
    if (!grant.verified || grant.sealed || grant.frozen) throw Conflict("Consultation grant is not active");

    Json context = Json::array();
    for (const auto& item : permit.channel.context)
        context.push_back({{"itemId", item.itemId}, {"text", item.text}, {"truncated", item.truncated}});
    const auto routing = intake(bot, "");
    if (routing.botId != bot) throw Conflict("Consultation Bot is unavailable");
    auto& db = *store_.p_;
    auto note = db.loadNotes(bot, soul, unixTime());
    const auto redactor = db.sharedRedactor();
    const SecretRedactor scoped({db.secrets.read(participationCredential(channel))});
    note.soul = scoped.text(redactor.text(note.soul));
    note.memory = scoped.text(redactor.text(note.memory));
    const ModelInput input{
        {{"botId",bot},{"request","Reply once to the person who invited you into their Session."}},
        {{"kind","session_consultation"},{"sourceMessageId",status.origin.invitation.sourceMessageId},
         {"sharedContext",std::move(context)}},
        Json::array(), std::move(note), {"reply"}, {}, Json::array(), routing.config
    };
    const auto turn = model.decide(input);
    if (turn.calls.size() != 1 || turn.calls.front().tool != "reply")
        throw Conflict("The Bot did not produce one consultation reply");
    const auto text = scoped.text(redactor.text(field(turn.calls.front().args, "text")));
    delivery.prepare(person, bot, channel, permit.commandId, text, status.origin.invitation.sourceMessageId);
    return delivery.deliver(person, bot, channel, permit.commandId);
}

SessionHandoffView SessionHandoff::step(const Person& person, Id channel, Model& model,
                                        const IntakeResolver& intake, const Runtime::SoulLoader& soul) {
    const auto current = view(person, channel);
    if (!current.coordination) throw Conflict("A consultation cannot coordinate work");
    if (current.opening) return resume(person, channel);
    RootDelivery delivery(store_, gate_);
    const auto grant = delivery.observeGrant(person, channel);
    if (!grant.verified || grant.sealed || grant.frozen) return view(person, channel);
    const auto activation = ParticipationStore(store_).coordination(person, channel);
    if (!activation.acknowledgment) throw Conflict("Coordination is still opening");
    const auto& invitation = current.origin.invitation;
    const auto& bot = invitation.botId;
    auto& db = *store_.p_;

    // Recover a saved effect before asking the model for another decision.
    auto deliverPending = [&] {
        std::string command, event, text, kind, specialist, doneWhen, commit, resultCommit;
        {
            std::lock_guard lock(db.mutex);
            auto row = db.query(R"SQL(
SELECT d.command_id,d.event_id,d.text,d.kind,s.specialist,s.done_when,s.commit_sha,r.commit_sha
FROM handoff_decisions d LEFT JOIN handoff_specialist_intents s ON s.command_id=d.command_id
LEFT JOIN handoff_reviewed_results r ON r.command_id=d.command_id
WHERE d.channel_id=? AND d.generation=? AND d.kind IN ('root_guidance','delegate_specialist')
 AND NOT d.settled ORDER BY d.rowid LIMIT 1
)SQL", {channel, grant.generation});
            if (!row.next()) return false;
            command = row.string(0);
            event = row.string(1);
            text = row.string(2);
            kind = row.string(3);
            specialist = row.string(4);
            doneWhen = row.string(5);
            commit = row.string(6);
            resultCommit = row.string(7);
        }
        bool settled = false;
        if (kind == "delegate_specialist") {
            if (!runtime_) return true;
            const auto currentPin = delivery.inputCommit(person, channel);
            if (!currentPin) return true;
            if (*currentPin == commit)
                runtime_->startSpecialist(person, {channel, grant.generation, command,specialist,text,doneWhen,commit});
            settled = true;
        } else {
            if (!resultCommit.empty()) {
                const auto rootCommit = delivery.inputCommit(person,channel);
                if (!rootCommit) return true;
                if (*rootCommit != resultCommit) {
                    {
                        std::lock_guard lock(db.mutex);
                        settled = !db.query("SELECT 1 FROM root_deliveries WHERE channel_id=? AND command_id=?",
                                            {channel,command}).next();
                    }
                    if (!settled) {
                        const auto prior = delivery.reconcile(person,bot,channel,command);
                        settled = prior.state == RootDeliveryState::Applied || prior.state == RootDeliveryState::Failed ||
                                  prior.state == RootDeliveryState::Rejected || prior.state == RootDeliveryState::Prepared;
                        if (!settled) return true;
                    }
                }
            }
            if (!settled) {
                delivery.prepare(person, bot, channel, command, text, event);
                const auto result = delivery.deliver(person, bot, channel, command);
                settled = result.state == RootDeliveryState::Applied || result.state == RootDeliveryState::Failed ||
                          result.state == RootDeliveryState::Rejected;
            }
        }
        if (settled) {
            std::lock_guard lock(db.mutex);
            db.exec("UPDATE handoff_decisions SET settled=1 WHERE channel_id=? AND command_id=?",
                    {channel, command});
        }
        return true;
    };
    if (deliverPending()) return view(person, channel);

    const RootGrantRef ref{invitation.target, activation.binding.activationId, activation.acknowledgment->epoch};
    const auto credential = db.secrets.read(participationCredential(channel));
    struct Answer { std::string event, command, question, text; Id position; };
    std::optional<Answer> answer;
    std::optional<std::pair<Id,Json>> workResult;
    {
        std::lock_guard lock(db.mutex);
        auto row = db.query(R"SQL(
SELECT event_id,command_id,text,answer_text,position FROM handoff_decisions
WHERE channel_id=? AND generation=? AND kind='handoff_question' AND answer_text IS NOT NULL
AND NOT answer_consumed ORDER BY rowid LIMIT 1
)SQL", {channel,grant.generation});
        if (row.next()) answer = Answer{row.string(0),row.string(1),row.string(2),row.string(3),row.number(4)};
        if (!answer) {
            auto work = db.query(R"SQL(
SELECT j.id,j.data FROM specialist_delegations d JOIN journal j ON j.task_id=d.sender_task
WHERE d.channel_id=? AND d.generation=? AND j.kind='specialist_result'
AND NOT EXISTS (SELECT 1 FROM handoff_reviewed_results r WHERE r.channel_id=d.channel_id AND r.journal_id=j.id)
ORDER BY j.id LIMIT 1
)SQL", {channel,grant.generation});
            if (work.next()) workResult = {work.number(0),work.json(1)};
        }
    }
    const auto page = answer || workResult ? std::optional(RootProgressPage{ref.target,current.eventCursor,{}})
                                          : gate_.events(ref, current.eventCursor, credential);
    if (!page || page->target != ref.target || page->cursor < current.eventCursor ||
        page->events.size() > 20) return view(person, channel);
    const auto redactor = db.sharedRedactor();
    const SecretRedactor scoped({credential});
    auto clean = [&](std::string_view value) { return scoped.text(redactor.text(value)); };
    Id previous = current.eventCursor;
    Json events = Json::array();
    for (const auto& event : page->events) {
        if (event.position <= previous || event.position > page->cursor || event.id.empty() ||
            event.id.size() > 1024 || event.summary.size() > 8192)
            throw Conflict("T3 returned invalid coordination progress");
        scoped.requirePublic({event.id});
        redactor.requirePublic({event.id});
        previous = event.position;
        events.push_back({{"kind",clean(event.kind)},{"status",clean(event.status)},
                          {"summary",clean(event.summary)}});
    }
    if (!answer && !workResult && page->cursor == current.eventCursor) return view(person, channel);
    std::string kind = "wait_for_root", text, specialist, doneWhen;
    std::optional<std::string> pin;
    if (answer || workResult || !events.empty()) {
        const auto routing = intake(bot, "");
        if (routing.botId != bot) throw Conflict("Coordinator Bot is unavailable");
        auto note = db.loadNotes(bot, soul, unixTime());
        note.soul = clean(note.soul);
        note.memory = clean(note.memory);
        auto context = Json::array();
        for (const auto& item : activation.channel.context)
            context.push_back({{"text",clean(item.text)},{"truncated",item.truncated}});
        std::vector<std::string> tools{"root_guidance","handoff_question","wait_for_root"};
        auto peers = Json::array();
        auto resultEvidence = Json(nullptr);
        const auto peerScope = Scope::workDelegate(ref.target.environmentId, ref.target.threadId);
        if (runtime_ && peers_ && activation.binding.request.allowDelegation &&
            store_.access().permits(bot,peerScope,true)) {
            for (const auto& peer : peers_(bot)) {
                const auto& request = peer.intake;
                if (!peer.role.empty() && request.environmentId == ref.target.environmentId &&
                    request.projectId == ref.target.projectId &&
                    store_.access().permits(request.botId,{"environment",request.environmentId,"launch"},true))
                    peers.push_back({{"botId",clean(request.botId)},{"role",clean(peer.role)}});
            }
            if (!peers.empty()) pin = delivery.inputCommit(person, channel);
            if (pin) tools.push_back("delegate_specialist");
            else peers = Json::array();
        }
        if (workResult) {
            const auto& result = workResult->second;
            if (!pin) pin = delivery.inputCommit(person,channel);
            const bool sameInput = pin && *pin == field(result,"artifactVersion");
            if (!sameInput) tools.erase(std::ranges::find(tools,"root_guidance"));
            resultEvidence = {{"status",sameInput ? clean(result.value("workflowStatus",field(result,"status"))) : "stale"},
                              {"commit",clean(field(result,"artifactVersion"))},
                              {"report",sameInput ? clean(field(result,"report")) : "The previous input is no longer current."}};
            auto sources = Json::array();
            if (sameInput) for (const auto& item : result.value("references",Json::array())) {
                if (sources.size() == 8) break;
                sources.push_back({{"stage",clean(field(item,"stageId"))},{"verdict",clean(field(item,"verdict"))},
                                   {"runId",clean(field(item,"runId"))},{"itemId",clean(field(item,"itemId"))}});
            }
            resultEvidence["evidence"] = std::move(sources);
        }
        const ModelInput input{
            {{"botId",bot},{"goal",clean(activation.binding.request.goal)}},
            {{"kind","root_coordination"},{"progress",std::move(events)},{"sharedContext",std::move(context)},
             {"specialists",peers},{"answer",answer ? Json{{"question",clean(answer->question)},
                                                           {"text",clean(answer->text)}} : Json(nullptr)},
             {"specialistResult",std::move(resultEvidence)}},
            Json::array(), std::move(note), tools,
            {}, Json::array(), routing.config
        };
        const auto turn = model.decide(input);
        if (turn.calls.size() != 1) throw Conflict("The coordinator did not choose one action");
        kind = turn.calls.front().tool;
        text = bounded(clean(field(turn.calls.front().args,"text")),8192);
        if (std::ranges::find(tools,kind) == tools.end() ||
            (kind != "wait_for_root" && trimSessionQuestionText(text).empty()))
            throw Conflict("Invalid coordinator action");
        if (kind == "delegate_specialist") {
            specialist = field(turn.calls.front().args,"botId");
            doneWhen = bounded(clean(field(turn.calls.front().args,"doneWhen")),8192);
            if (doneWhen.empty() || !std::ranges::any_of(peers,[&](const Json& item) {
                return item.at("botId") == specialist;
            })) throw Conflict("Invalid specialist selection");
        }
    }
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.authorizeParticipation(person, channel);
        auto row = db.query(R"SQL(
SELECT h.event_cursor FROM session_handoffs h JOIN root_gate_channels g ON g.channel_id=h.channel_id
WHERE h.channel_id=? AND g.generation=? AND NOT g.sealed AND NOT g.frozen
)SQL", {channel, grant.generation});
        if (!row.next() || row.number(0) != current.eventCursor) {
            tx.commit();
            return view(person, channel);
        }
        if (answer && !db.query(R"SQL(
SELECT 1 FROM handoff_decisions WHERE channel_id=? AND event_id=? AND generation=? AND NOT answer_consumed
)SQL", {channel,answer->event,grant.generation}).next()) {
            tx.commit();
            return view(person,channel);
        }
        if (workResult && db.query("SELECT 1 FROM handoff_reviewed_results WHERE channel_id=? AND journal_id=?",
                                  {channel,workResult->first}).next()) {
            tx.commit();
            return view(person,channel);
        }
        if (answer || workResult || !page->events.empty()) {
            const auto event = answer ? "flock-answer:" + answer->command :
                workResult ? "flock-result:" + std::to_string(workResult->first) : page->events.back().id;
            const auto position = answer ? answer->position : workResult ? current.eventCursor : page->events.back().position;
            const auto command = actionId();
            db.exec(R"SQL(
INSERT INTO handoff_decisions(channel_id,event_id,position,generation,command_id,kind,text,settled)
VALUES(?,?,?,?,?,?,?,?)
)SQL", {channel, event, position, grant.generation, command, kind, text,
         static_cast<int>(kind != "root_guidance" && kind != "delegate_specialist")});
            if (kind == "delegate_specialist")
                db.exec("INSERT INTO handoff_specialist_intents VALUES(?,?,?,?)",{command,specialist,doneWhen,*pin});
            if (answer) db.exec("UPDATE handoff_decisions SET answer_consumed=1 WHERE channel_id=? AND event_id=?",
                                {channel,answer->event});
            if (workResult) db.exec("INSERT INTO handoff_reviewed_results VALUES(?,?,?,?)",
                                    {channel,workResult->first,command,field(workResult->second,"artifactVersion")});
        }
        db.exec("UPDATE session_handoffs SET event_cursor=? WHERE channel_id=?", {page->cursor, channel});
        tx.commit();
    }
    if (kind == "root_guidance" || kind == "delegate_specialist") deliverPending();
    return view(person, channel);
}

std::vector<std::pair<Id,std::string>> SessionHandoff::pending(Id after) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto rows = db.query(R"SQL(
SELECT c.id,c.person_id FROM return_channels c
JOIN session_handoffs h ON h.channel_id=c.id
LEFT JOIN root_gate_channels g ON g.channel_id=c.id
LEFT JOIN coordination_activations a ON a.channel_id=c.id
LEFT JOIN consult_permits p ON p.channel_id=c.id
LEFT JOIN root_deliveries d ON d.channel_id=c.id AND d.command_id=p.command_id
WHERE c.id>? AND (g.grant_id IS NULL OR (a.channel_id IS NOT NULL AND NOT g.release_acknowledged) OR
 (p.channel_id IS NOT NULL AND ((NOT g.sealed AND NOT g.frozen AND d.state IS NULL) OR d.state IN (0,1,2,6))))
ORDER BY c.id LIMIT 16
)SQL", {after});
    std::vector<std::pair<Id,std::string>> result;
    while (rows.next()) result.emplace_back(rows.number(0),rows.string(1));
    return result;
}

}
