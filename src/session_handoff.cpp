#include "flock/session_handoff.hpp"
#include "flock/bot_profile.hpp"
#include "storage.hpp"

#include <algorithm>

namespace flockv0 {
namespace {

using State = RootDeliveryState;
constexpr Id decisionBudget = 24;

const ReturnChannel& channelOf(const Participation& participation) {
    return std::visit([](const auto& entry) -> const ReturnChannel& { return entry.channel; },participation);
}

Json workQuestion(const Json& task, const SecretRedactor& scrub) {
    const auto& wait = task.at("wait");
    if (task.at("state") != "waiting" || !wait.is_object() || field(wait,"reason") != "user_question")
        return nullptr;
    return {{"waitId",wait.at("id")},{"text",redactT3Tokens(scrub.text(field(wait,"text")))}};
}

bool unresolved(State state) {
    return state == State::Dispatching || state == State::Accepted || state == State::Queued ||
           state == State::Unknown;
}

enum class ModelAttemptState { Reserved, Exhausted, Stale };

template <typename Database>
ModelAttemptState reserveModelAttempt(Database& db, const Person& person, Id channel, Id generation,
                                      const Json& config, std::string_view purpose, Id& attempt) {
    Transaction transaction(db.db);
    db.authorizeParticipation(person,channel);
    auto control = db.query("SELECT generation,state FROM session_controls WHERE channel_id=?",{channel});
    if (!control.next() || control.number(0) != generation || control.string(1) != "active") {
        transaction.commit();
        return ModelAttemptState::Stale;
    }
    const auto budget = db.ensureModelBudget("session-handoff/" + std::to_string(channel),config);
    attempt = db.reserveModelCall(budget,purpose,0,0,0);
    if (!attempt) {
        db.exec(R"SQL(
UPDATE session_controls SET state='budget_exhausted',generation=generation+1
WHERE channel_id=? AND generation=? AND state='active'
)SQL",{channel,generation});
        if (sqlite3_changes(db.db)) {
            db.exec("UPDATE thread_deliveries SET state=? WHERE channel_id=? AND state=?",
                    {static_cast<int>(State::Rejected),channel,static_cast<int>(State::Prepared)});
            db.exec("UPDATE session_decisions SET settled=1 WHERE channel_id=? AND NOT settled",{channel});
        }
        transaction.commit();
        return ModelAttemptState::Exhausted;
    }
    transaction.commit();
    return ModelAttemptState::Reserved;
}

}

void Store::Impl::initializeSessionHandoffSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS session_decisions(
 channel_id INTEGER NOT NULL REFERENCES session_controls(channel_id),
 event_id TEXT NOT NULL, position INTEGER NOT NULL, generation INTEGER NOT NULL,
 command_id TEXT NOT NULL UNIQUE, kind TEXT NOT NULL, text TEXT NOT NULL, settled INTEGER NOT NULL,
 answer_text TEXT, answer_consumed INTEGER NOT NULL DEFAULT 0,
 specialist TEXT NOT NULL, done_when TEXT NOT NULL, commit_sha TEXT NOT NULL, evidence_sha TEXT NOT NULL,
 journal_id INTEGER UNIQUE REFERENCES journal(id),
 PRIMARY KEY(channel_id,event_id))
)SQL");
}

void SessionHandoff::recover() {
    auto& db = *store_.p_;
    struct Sweep { std::string person; Id channel, generation; };
    std::vector<Sweep> sweeps;
    {
        std::lock_guard lock(db.mutex);
        db.exec("UPDATE thread_deliveries SET state=? WHERE state=?",
                {static_cast<int>(State::Unknown),static_cast<int>(State::Dispatching)});
        if (runtime_) {
            auto rows = db.query(R"SQL(
SELECT DISTINCT r.person_id,d.channel_id,c.generation-1 FROM session_controls c
JOIN return_channels r ON r.id=c.channel_id
JOIN specialist_delegations d ON d.channel_id=c.channel_id
WHERE c.state IN ('paused','stopped','budget_exhausted') AND d.generation<c.generation
)SQL");
            while (rows.next()) sweeps.push_back({rows.string(0),rows.number(1),rows.number(2)});
        }
    }
    for (const auto& sweep : sweeps)
        if (const auto person=people_(sweep.person))
            runtime_->freezeSpecialistWork(*person,sweep.channel,sweep.generation);
}

SessionHandoffView SessionHandoff::freezeAs(const Person& person, Id channel, std::string_view state) {
    auto& db = *store_.p_;
    Id generation = 0;
    bool changed = false, coordination = false;
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        const auto participation = db.authorizeParticipation(person,channel,false);
        coordination = std::holds_alternative<CoordinationActivation>(participation);
        if (state == "paused" && !std::holds_alternative<CoordinationActivation>(participation))
            throw Conflict("Only coordination can be paused");
        auto control = db.query("SELECT generation FROM session_controls WHERE channel_id=?",{channel});
        if (control.next()) generation = control.number(0);
        db.exec(R"SQL(
UPDATE session_controls SET generation=generation+1,state=?
WHERE channel_id=? AND (state='active' OR (?='stopped' AND state='paused'))
)SQL",{state,channel,state});
        changed = sqlite3_changes(db.db) != 0;
        if (changed) {
            db.exec("UPDATE thread_deliveries SET state=? WHERE channel_id=? AND state=?",
                    {static_cast<int>(State::Rejected),channel,static_cast<int>(State::Prepared)});
            db.exec("UPDATE session_decisions SET settled=1 WHERE channel_id=? AND NOT settled",{channel});
        }
        tx.commit();
    }
    if (changed && coordination && runtime_ && (state=="paused" || state=="stopped" || state=="budget_exhausted"))
        runtime_->freezeSpecialistWork(person,channel,generation);
    return view(person,channel);
}

SessionHandoffView SessionHandoff::pause(const Person& person, Id channel) {
    return freezeAs(person,channel,"paused");
}

SessionHandoffView SessionHandoff::freeze(const Person& person, Id channel) {
    return freezeAs(person,channel,"stopped");
}

SessionHandoffView SessionHandoff::resume(const Person& person, Id channel) {
    if (runtime_) {
        const auto current = view(person,channel);
        if (current.state=="paused" && current.generation>0)
            runtime_->freezeSpecialistWork(person,channel,current.generation-1);
    }
    RootDelivery(store_,session_).reconcile(person,channel);
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    db.authorizeParticipation(person,channel);
    if (db.query("SELECT 1 FROM thread_deliveries WHERE channel_id=? AND state IN (1,2,3,7)",{channel}).next())
        throw Conflict("An earlier Session input is still unresolved");
    db.exec(R"SQL(
UPDATE session_controls SET state='active',initial_pending=1
WHERE channel_id=? AND state='paused'
)SQL",{channel});
    if (!sqlite3_changes(db.db)) throw Conflict("This coordination cannot be resumed");
    tx.commit();
    return view(person,channel);
}

SessionHandoffView SessionHandoff::answer(const Person& person, Id channel, Id generation,
                                          std::string_view eventId, std::string_view text) {
    if (eventId.empty() || eventId.size() > 1024 || text.size() > 8192)
        throw Conflict("This question is unavailable");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    db.authorizeParticipation(person,channel);
    const auto cleaned = redactT3Tokens(db.sharedRedactor().text(text));
    if (trimSessionQuestionText(cleaned).empty()) throw Conflict("The answer is empty");
    auto row = db.query(R"SQL(
SELECT d.answer_text FROM session_decisions d JOIN session_controls s ON s.channel_id=d.channel_id
WHERE d.channel_id=? AND d.event_id=? AND d.generation=? AND d.kind='handoff_question'
AND s.generation=d.generation AND s.state='active'
)SQL",{channel,eventId,generation});
    if (!row.next() || (!row.string(0).empty() && row.string(0) != cleaned))
        throw Conflict("This question is unavailable");
    db.exec("UPDATE session_decisions SET answer_text=? WHERE channel_id=? AND event_id=? AND answer_text IS NULL",
            {cleaned,channel,eventId});
    tx.commit();
    return view(person,channel);
}

SessionHandoffView SessionHandoff::answerWork(const Person& person, Id channel, Id generation,
                                              std::string_view delegation, std::string_view stage, Id wait,
                                              std::string_view text) {
    if (wait <= 0 || text.size() > 8000) throw Conflict("This work question is unavailable");
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const auto participation = db.authorizeParticipation(person,channel);
    const auto* coordination = std::get_if<CoordinationActivation>(&participation);
    if (!coordination || !coordination->binding.request.allowDelegation) throw Conflict("Delegation is unavailable");
    auto binding = db.query(R"SQL(
SELECT CASE WHEN ?4='' THEN d.sender_task ELSE s.task_id END
FROM specialist_delegations d JOIN session_controls c ON c.channel_id=d.channel_id
LEFT JOIN workflows w ON w.delegation_id=d.id
LEFT JOIN workflow_stages s ON s.workflow_id=w.id AND s.stage_id=?4
WHERE d.channel_id=?1 AND d.generation=?2 AND d.id=?3 AND c.state='active'
AND (d.generation=c.generation OR (d.generation<c.generation AND EXISTS (
 SELECT 1 FROM tasks admitted WHERE admitted.id=CASE WHEN ?4='' THEN d.recipient_task ELSE s.task_id END
 AND json_extract(admitted.data,'$.activeRun') IS NOT NULL)))
)SQL",{channel,generation,delegation,stage});
    if (!binding.next() || !binding.number(0)) throw Conflict("This work question is unavailable");
    const auto task = db.load(binding.number(0));
    const auto question = workQuestion(task,db.sharedRedactor());
    if (question.is_null() || question.at("waitId") != wait) throw Conflict("This work question is unavailable");
    const auto cleaned = redactT3Tokens(db.sharedRedactor().text(text));
    db.operate(person,TaskTarget::from(task,wait),"answer",{{"text",cleaned}});
    tx.commit();
    return view(person,channel);
}

SessionHandoffView SessionHandoff::view(const Person& person, Id channel) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db,Transaction::Mode::read);
    const auto participation = db.authorizeParticipation(person,channel,false);
    const auto& returnChannel = channelOf(participation);
    const auto& origin = returnChannel.origin;
    const auto scrub = db.sharedRedactor();
    const auto clean = [&](std::string_view text) { return redactT3Tokens(scrub.text(text)); };
    auto row = db.query("SELECT state,generation,event_cursor FROM session_controls WHERE channel_id=?",{channel});
    if (!row.next()) throw Conflict("Unknown Session handoff");
    SessionHandoffView result;
    result.channel = channel;
    result.origin = origin;
    result.state = row.string(0);
    result.generation = row.number(1);
    result.eventCursor = row.number(2);
    result.frozen = result.state != "active";
    if (const auto* coord = std::get_if<CoordinationActivation>(&participation)) {
        result.coordination = true;
        result.permission = coord->binding.request;
        result.permission->goal = clean(result.permission->goal);
    }
    if (auto decision = db.query(R"SQL(
SELECT event_id,position,kind,text,settled FROM session_decisions
WHERE channel_id=? AND kind<>'wait_for_root' ORDER BY rowid DESC LIMIT 1
)SQL",{channel}); decision.next()) {
        result.latestDecision = {{"eventId",decision.string(0)},{"position",decision.number(1)},
                                 {"kind",decision.string(2)},{"text",clean(decision.string(3))},
                                 {"settled",decision.number(4) != 0}};
    }
    if (!result.frozen) {
        auto question = db.query(R"SQL(
SELECT event_id,text FROM session_decisions WHERE channel_id=? AND generation=? AND kind='handoff_question'
AND answer_text IS NULL ORDER BY rowid LIMIT 1
)SQL",{channel,result.generation});
        if (question.next()) result.pendingQuestion = {{"eventId",question.string(0)},{"text",clean(question.string(1))}};
    }
    auto delegations = db.query(R"SQL(
SELECT d.id,r.bot_id,r.state,d.purpose,d.done_when,d.commit_sha,w.id,j.data,p.phase,d.sender_task,d.parent_id,d.generation,d.recipient_task
FROM specialist_delegations d JOIN tasks r ON r.id=d.recipient_task
JOIN peer_exchanges p ON p.id=d.id LEFT JOIN workflows w ON w.delegation_id=d.id
LEFT JOIN journal j ON j.task_id=d.sender_task AND j.kind='specialist_result'
WHERE d.channel_id=? ORDER BY d.rowid LIMIT 16
)SQL", {channel});
    while (delegations.next()) {
        Json work{{"delegationId",delegations.string(0)},{"executor",delegations.string(1)},
                  {"state",delegations.string(2)},{"generation",delegations.number(11)},{"purpose",clean(delegations.string(3))},
                  {"doneWhen",clean(delegations.string(4))},{"commit",delegations.string(5)},
                  {"phase",delegations.string(8)},{"stages",Json::array()},
                  {"question",workQuestion(db.load(delegations.number(9)),scrub)}};
        const auto answerable = [&](Id task) {
            return !result.frozen && (delegations.number(11)==result.generation ||
                (delegations.number(11)<result.generation && task>0 &&
                 db.query("SELECT 1 FROM tasks WHERE id=? AND json_extract(data,'$.activeRun') IS NOT NULL",{task}).next()));
        };
        work["canAnswer"] = answerable(delegations.number(12));
        if (const auto parent = delegations.string(10); !parent.empty()) work["parentDelegationId"] = parent;
        if (const auto workflow = delegations.number(6)) {
            const auto details = db.workflowStatus(workflow,delegations.string(1),unixTime());
            for (const auto& stage : details.at("stages")) {
                const auto task = stage.at("taskId").get<Id>();
                work["stages"].push_back({{"stageId",stage.at("stageId")},
                    {"name",clean(stage.at("name").get<std::string>())},
                    {"status",stage.at("status")},{"needsAttention",stage.at("needsAttention")},
                    {"commit",stage.at("inputArtifactVersion")},
                    {"question",task ? workQuestion(db.load(task),scrub) : Json(nullptr)},
                    {"canAnswer",answerable(task)}});
            }
        }
        if (!delegations.string(7).empty()) {
            const auto settled = delegations.json(7);
            work["result"] = {{"status",clean(settled.value("workflowStatus",field(settled,"status")))},
                              {"verdict",clean(field(settled,"verdict"))},{"report",clean(field(settled,"report"))}};
            auto evidence = Json::array();
            for (const auto& source : settled.value("references",Json::array())) {
                evidence.push_back({{"threadId",field(source,"threadId")},{"runId",field(source,"runId")},
                                    {"itemId",field(source,"itemId")},{"stageId",field(source,"stageId")},
                                    {"verdict",clean(field(source,"verdict"))},{"commit",field(source,"artifactVersion")},
                                    {"findings",clean(field(source,"findings"))}});
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
    auto deliveries = db.query(R"SQL(
SELECT command_id,state,message_id,run_id,mode,observed_run_id FROM thread_deliveries
WHERE channel_id=? ORDER BY rowid DESC LIMIT 24
)SQL",{channel});
    while (deliveries.next()) {
        RootDeliveryView delivery{deliveries.string(0),static_cast<State>(deliveries.number(1)),{},deliveries.string(5)};
        if (!deliveries.string(2).empty())
            delivery.receipt = ThreadReceipt{deliveries.string(2),deliveries.string(3),deliveries.string(4)};
        if (result.frozen && unresolved(delivery.state)) result.residualCommandIds.push_back(delivery.commandId);
        result.deliveries.push_back(std::move(delivery));
    }
    tx.commit();
    return result;
}

std::vector<SessionHandoffView> SessionHandoff::recent(const Person& person) const {
    auto& db = *store_.p_;
    std::vector<Id> channels;
    {
        std::lock_guard lock(db.mutex);
        auto rows = db.query(R"SQL(
SELECT s.channel_id FROM session_controls s JOIN return_channels c ON c.id=s.channel_id
WHERE c.person_id=? ORDER BY s.channel_id DESC LIMIT 25
)SQL",{person.id});
        while (rows.next()) channels.push_back(rows.number(0));
    }
    std::vector<SessionHandoffView> result;
    for (const auto channel : channels) {
        try { result.push_back(view(person,channel)); }
        catch (const Conflict&) {}
    }
    return result;
}

void SessionHandoff::consult(const Person& person, Id channel, Model& model,
                              const Runtime::SoulLoader& soul) {
    RootDelivery delivery(store_,session_);
    delivery.reconcile(person,channel);
    const auto current = view(person,channel);
    if (current.coordination) throw Conflict("This Session is authorized for coordination");
    if (current.frozen) return;
    auto& db = *store_.p_;
    const auto permit = [&] {
        std::lock_guard lock(db.mutex);
        return std::get<ConsultPermit>(db.authorizeParticipation(person,channel));
    }();
    const auto& origin = permit.channel.origin.invitation;
    if (current.deliveries.empty()) {
        const auto routing = resolveBot(origin.botId);
        auto notes = db.loadNotes(origin.botId,soul,unixTime());
        Id attempt = 0;
        ModelAttemptState attemptState;
        {
            std::lock_guard lock(db.mutex);
            attemptState = reserveModelAttempt(db,person,channel,current.generation,routing.config,
                                               "session_consultation",attempt);
        }
        if (attemptState != ModelAttemptState::Reserved) {
            if (attemptState == ModelAttemptState::Exhausted && runtime_)
                runtime_->freezeSpecialistWork(person,channel,current.generation);
            return;
        }
        const auto scrub = db.sharedRedactor();
        notes.soul = redactT3Tokens(scrub.text(notes.soul));
        notes.memory = redactT3Tokens(scrub.text(notes.memory));
        const ModelInput input{
            {{"botId",origin.botId},{"request","Reply once to the paired Session's request."}},
            {{"kind","session_consultation"},{"request",redactT3Tokens(scrub.text(permit.channel.request))}},
            Json::array(),std::move(notes),{"reply"},{},Json::array(),routing.config
        };
        const auto turn = db.callModel(origin.botId,model,input,attempt);
        if (turn.calls.size() != 1 || !turn.calls.front().rejection.empty() || turn.calls.front().tool != "reply")
            throw Conflict("The Bot did not produce one consultation reply");
        const auto text = bounded(scrub.text(field(turn.calls.front().args,"text")),8192);
        if (trimSessionQuestionText(text).empty()) throw Conflict("The consultation reply is empty");
        std::lock_guard lock(db.mutex);
        db.authorizeParticipation(person,channel);
        delivery.prepare(channel,current.generation,permit.commandId,origin.botId,origin.requestId,text,current.eventCursor);
    }
    const auto reply = delivery.deliver(person,channel,permit.commandId);
    if (reply.state == State::Observed) freezeAs(person,channel,"completed");
    else if (reply.state == State::Rejected || reply.state == State::Failed) freezeAs(person,channel,"failed");
}

SessionHandoffView SessionHandoff::step(const Person& person, Id channel, Model& model,
                                        const Runtime::SoulLoader& soul) {
    RootDelivery delivery(store_,session_);
    delivery.reconcile(person,channel);
    const auto current = view(person,channel);
    if (!current.coordination) throw Conflict("A consultation cannot coordinate work");
    if (current.frozen || !current.pendingQuestion.is_null()) return current;
    const auto& invitation = current.origin.invitation;
    const auto& target = invitation.target;
    const auto& bot = invitation.botId;
    auto& db = *store_.p_;

    struct Effect {
        std::string command, text, kind, specialist, doneWhen, commit, evidence;
        State delivery;
    };
    auto recoverEffect = [&]() {
        std::optional<Effect> effect;
        {
            std::lock_guard lock(db.mutex);
            auto row = db.query(R"SQL(
SELECT s.command_id,s.text,s.kind,s.specialist,s.done_when,s.commit_sha,s.evidence_sha,d.state
FROM session_decisions s LEFT JOIN thread_deliveries d ON d.command_id=s.command_id
WHERE s.channel_id=? AND s.generation=? AND NOT s.settled
AND s.kind IN ('root_guidance','delegate_specialist') ORDER BY s.rowid LIMIT 1
)SQL",{channel,current.generation});
            if (row.next())
                effect = Effect{row.string(0),row.string(1),row.string(2),row.string(3),row.string(4),row.string(5),row.string(6),static_cast<State>(row.number(7))};
        }
        if (!effect) return false;
        const auto& job = *effect;
        const auto neededPin = job.kind == "delegate_specialist" ? job.commit : job.evidence;
        if (!neededPin.empty() && (job.kind == "delegate_specialist" || job.delivery == State::Prepared)) {
            const auto pin = session_.inputCommit(target);
            if (!pin) return true;
            if (*pin != neededPin) {
                std::lock_guard lock(db.mutex);
                db.exec("UPDATE thread_deliveries SET state=? WHERE command_id=? AND state=?",
                        {static_cast<int>(State::Rejected),job.command,static_cast<int>(State::Prepared)});
                db.exec("UPDATE session_decisions SET settled=1,journal_id=NULL WHERE command_id=?",{job.command});
                return false;
            }
        }
        if (job.kind == "delegate_specialist") {
            if (!runtime_) return true;
            try {
                runtime_->startSpecialist(person,{channel,current.generation,job.command,job.specialist,job.text,job.doneWhen,job.commit});
            } catch (const SpecialistAllowanceExhausted&) {
                {
                    std::lock_guard lock(db.mutex);
                    Transaction tx(db.db);
                    db.exec("UPDATE session_decisions SET settled=1 WHERE command_id=?",{job.command});
                    tx.commit();
                }
                freezeAs(person,channel,"budget_exhausted");
                return true;
            }
        } else {
            const auto result = delivery.deliver(person,channel,job.command);
            if (result.state == State::Prepared || unresolved(result.state)) return true;
        }
        std::lock_guard lock(db.mutex);
        db.exec("UPDATE session_decisions SET settled=1 WHERE command_id=?",{job.command});
        return false;
    };
    if (recoverEffect()) return view(person,channel);
    bool exhausted;
    {
        std::lock_guard lock(db.mutex);
        auto count = db.query("SELECT count(*) FROM session_decisions WHERE channel_id=?",{channel});
        count.next();
        exhausted = count.number(0) >= decisionBudget;
    }
    if (exhausted) return freezeAs(person,channel,"budget_exhausted");

    struct Answer { std::string event, command, question, text; };
    std::optional<Answer> answer;
    std::optional<std::pair<Id,Json>> workResult;
    bool initial;
    std::string auth, request;
    {
        std::lock_guard lock(db.mutex);
        const auto participation = db.authorizeParticipation(person,channel);
        request = channelOf(participation).request;
        auto control = db.query("SELECT auth_generation,initial_pending FROM session_controls WHERE channel_id=?",{channel});
        control.next();
        auth = control.string(0);
        initial = control.number(1) != 0;
        auto row = db.query(R"SQL(
SELECT event_id,command_id,text,answer_text FROM session_decisions
WHERE channel_id=? AND generation=? AND kind='handoff_question' AND answer_text IS NOT NULL
AND NOT answer_consumed ORDER BY rowid LIMIT 1
)SQL",{channel,current.generation});
        if (row.next()) answer = Answer{row.string(0),row.string(1),row.string(2),row.string(3)};
        if (!answer) {
            auto work = db.query(R"SQL(
SELECT j.id,j.data FROM specialist_delegations d JOIN journal j ON j.task_id=d.sender_task
WHERE d.channel_id=? AND d.generation=? AND d.parent_id IS NULL AND j.kind='specialist_result'
AND NOT EXISTS (SELECT 1 FROM session_decisions r WHERE r.journal_id=j.id)
ORDER BY j.id LIMIT 1
)SQL",{channel,current.generation});
            if (work.next()) workResult = {work.number(0),work.json(1)};
        }
    }
    const auto page = session_.read(target,current.eventCursor);
    if (!page || page->target != target || page->cursor < current.eventCursor) return current;
    if (page->authGeneration != auth) return freezeAs(person,channel,"stopped");
    const auto scrub = db.sharedRedactor();
    auto clean = [&](std::string_view text) { return redactT3Tokens(scrub.text(text)); };
    Id humanInput = 0;
    Json events = Json::array();
    std::string lastEvent;
    for (const auto& event : page->events) {
        if (event.position <= current.eventCursor || event.position > page->cursor || event.id.empty()) continue;
        if (event.kind != "assistant_message" && !(event.kind == "user_message" && event.createdBy == "user")) continue;
        if (event.text.empty() || (event.kind == "assistant_message" && event.status != "completed")) continue;
        if (event.kind == "user_message") humanInput = std::max(humanInput,event.position);
        events.push_back({{"kind",event.kind},{"summary",bounded(clean(event.text),8192)}});
        lastEvent = event.id;
    }
    if (!initial && !answer && !workResult && events.empty()) {
        std::lock_guard lock(db.mutex);
        db.exec(R"SQL(
UPDATE session_controls SET event_cursor=? WHERE channel_id=? AND generation=? AND state='active' AND event_cursor=?
)SQL",{page->cursor,channel,current.generation,current.eventCursor});
        return view(person,channel);
    }
    const auto routing = resolveBot(bot);
    auto note = db.loadNotes(bot,soul,unixTime());
    note.soul = clean(note.soul);
    note.memory = clean(note.memory);
    std::vector<std::string> tools{"root_guidance","handoff_question","wait_for_root","complete_handoff"};
    auto peers = Json::array();
    auto work = Json::array();
    std::optional<std::string> pin;
    const bool hasPriorEvidence = std::ranges::any_of(current.work,[](const auto& assignment) {
        return assignment.contains("result");
    });
    if (workResult || hasPriorEvidence) pin = session_.inputCommit(target);
    bool awaitingSpecialist = false;
    for (const auto& assignment : current.work) {
        if (assignment.contains("parentDelegationId")) continue;
        const auto result = assignment.find("result");
        awaitingSpecialist |= assignment.at("generation") == current.generation && result == assignment.end();
        const auto commit = field(assignment,"commit");
        const auto status = result == assignment.end() ? field(assignment,"state") :
            pin && *pin == commit ? field(*result,"status") : std::string("stale");
        work.push_back({{"executor",clean(field(assignment,"executor"))},
                        {"purpose",clean(field(assignment,"purpose"))},
                        {"commit",clean(commit)},{"status",clean(status)}});
    }
    if (runtime_ && runtime_->specialistBudgetRemaining(channel)>=2 && peers_ &&
        current.permission->allowDelegation && !awaitingSpecialist) {
        for (const auto& peer : peers_(bot)) {
            const auto& settings = peer.intake;
            if (validBotRole(peer.role) && settings.environmentId == target.environmentId &&
                settings.projectId == target.projectId)
                peers.push_back({{"botId",clean(settings.botId)},{"role",clean(peer.role)}});
        }
        if (!peers.empty() && !workResult && !hasPriorEvidence) pin = session_.inputCommit(target);
        if (pin) tools.push_back("delegate_specialist");
        else peers = Json::array();
    }
    if (awaitingSpecialist) std::erase(tools,"complete_handoff");
    auto evidence = Json(nullptr);
    std::string evidenceSha;
    if (workResult) {
        const auto& result = workResult->second;
        const bool sameInput = pin && *pin == field(result,"artifactVersion");
        if (sameInput) evidenceSha = *pin;
        else {
            std::erase(tools,"root_guidance");
            std::erase(tools,"complete_handoff");
        }
        evidence = {{"status",sameInput ? clean(result.value("workflowStatus",field(result,"status"))) : "stale"},
                    {"commit",clean(field(result,"artifactVersion"))},
                    {"report",sameInput ? clean(field(result,"report")) : "The previous input is no longer current."}};
        auto sources = Json::array();
        if (sameInput) for (const auto& item : result.value("references",Json::array())) {
            if (sources.size() == 8) break;
            sources.push_back({{"stage",clean(field(item,"stageId"))},{"verdict",clean(field(item,"verdict"))},
                               {"runId",clean(field(item,"runId"))},{"itemId",clean(field(item,"itemId"))},
                               {"findings",clean(field(item,"findings"))}});
        }
        evidence["evidence"] = std::move(sources);
    }
    const ModelInput input{
        {{"botId",bot},{"goal",clean(current.permission->goal)}},
        {{"kind","root_coordination"},{"request",clean(request)},{"progress",std::move(events)},
         {"specialists",peers},{"specialistWork",std::move(work)},
         {"answer",answer ? Json{{"question",clean(answer->question)},{"text",clean(answer->text)}} : Json(nullptr)},
         {"specialistResult",std::move(evidence)}},
        Json::array(),std::move(note),tools,{},Json::array(),routing.config
    };
    Id attempt = 0;
    ModelAttemptState attemptState;
    {
        std::lock_guard lock(db.mutex);
        attemptState = reserveModelAttempt(db,person,channel,current.generation,routing.config,
                                           "session_coordination",attempt);
    }
    if (attemptState != ModelAttemptState::Reserved) {
        if (attemptState == ModelAttemptState::Exhausted && runtime_)
            runtime_->freezeSpecialistWork(person,channel,current.generation);
        return view(person,channel);
    }
    const auto turn = db.callModel(bot,model,input,attempt);
    if (turn.calls.size() != 1 || !turn.calls.front().rejection.empty())
        throw Conflict("The coordinator did not choose one action");
    const auto& decision = turn.calls.front();
    const auto& kind = decision.tool;
    const auto text = bounded(clean(field(decision.args,"text")),8192);
    if (std::ranges::find(tools,kind) == tools.end() ||
        (kind != "wait_for_root" && trimSessionQuestionText(text).empty()))
        throw Conflict("Invalid coordinator action");
    std::string specialist, doneWhen;
    if (kind == "delegate_specialist") {
        specialist = field(decision.args,"botId");
        doneWhen = bounded(clean(field(decision.args,"doneWhen")),8192);
        if (doneWhen.empty() || !std::ranges::any_of(peers,[&](const Json& item) { return item.at("botId") == specialist; }))
            throw Conflict("Invalid specialist selection");
    }
    if (kind == "complete_handoff" && !evidenceSha.empty()) {
        const auto latestPin = session_.inputCommit(target);
        if (!latestPin || *latestPin != evidenceSha) return view(person,channel);
    }
    const bool effect = kind == "root_guidance" || kind == "delegate_specialist";
    {
        std::lock_guard lock(db.mutex);
        Transaction tx(db.db);
        db.authorizeParticipation(person,channel);
        auto row = db.query(R"SQL(
SELECT event_cursor,initial_pending FROM session_controls WHERE channel_id=? AND generation=? AND state='active'
)SQL",{channel,current.generation});
        if (!row.next() || row.number(0) != current.eventCursor || (row.number(1) != 0) != initial) {
            tx.commit();
            return view(person,channel);
        }
        const auto event = answer ? "flock-answer:" + answer->command :
            workResult ? "flock-result:" + std::to_string(workResult->first) + ":" + pin.value_or("unavailable") :
            initial ? "flock-activation:" + std::to_string(current.generation) : lastEvent;
        const auto command = actionId();
        db.exec(R"SQL(
INSERT OR IGNORE INTO session_decisions(channel_id,event_id,position,generation,command_id,kind,text,settled,
specialist,done_when,commit_sha,evidence_sha,journal_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)
)SQL",{channel,event,page->cursor,current.generation,command,kind,text,static_cast<int>(!effect),
        specialist,doneWhen,kind == "delegate_specialist" ? *pin : "",evidenceSha,
        workResult ? Json(workResult->first) : Json(nullptr)});
        if (sqlite3_changes(db.db)) {
            db.exec("UPDATE session_controls SET human_input_position=max(human_input_position,?) WHERE channel_id=?",
                    {humanInput,channel});
            if (kind == "root_guidance")
                delivery.prepare(channel,current.generation,command,bot,invitation.requestId,text,page->cursor);
            if (answer)
                db.exec("UPDATE session_decisions SET answer_consumed=1 WHERE channel_id=? AND event_id=?",
                        {channel,answer->event});
            if (kind == "complete_handoff")
                db.exec("UPDATE session_controls SET state='completed',generation=generation+1 WHERE channel_id=?",{channel});
        }
        db.exec("UPDATE session_controls SET event_cursor=?,initial_pending=0 WHERE channel_id=?",{page->cursor,channel});
        tx.commit();
    }
    if (effect) recoverEffect();
    return view(person,channel);
}

std::vector<std::pair<Id,std::string>> SessionHandoff::pending(Id after) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    auto rows = db.query(R"SQL(
SELECT c.id,c.person_id FROM return_channels c JOIN session_controls s ON s.channel_id=c.id
WHERE c.id>? AND (s.state='active' OR EXISTS(
 SELECT 1 FROM thread_deliveries d WHERE d.channel_id=c.id AND d.state IN (1,2,3,7)))
ORDER BY c.id LIMIT 16
)SQL",{after});
    std::vector<std::pair<Id,std::string>> result;
    while (rows.next()) result.emplace_back(rows.number(0),rows.string(1));
    return result;
}

}
