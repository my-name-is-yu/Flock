#include "flock/participation.hpp"
#include "storage.hpp"

#include <algorithm>
#include <set>

namespace flockv0 {
namespace {

constexpr std::size_t maxContextItems = 3;
constexpr std::size_t maxContextBytes = 8192;

bool validId(std::string_view value) {
    return !value.empty() && value.size() <= 1024 &&
           std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

void validateInvitation(const InvitationRef& ref) {
    const auto& target = ref.target;
    if (!validId(target.environmentId) || !validId(target.projectId) || !validId(target.threadId) ||
        !validId(target.runId) || !validId(ref.requestId) || !validId(ref.sourceMessageId) || !validBotId(ref.botId))
        throw Conflict("Invalid Session invitation");
}

void authorize(const Access& access, const Person& person, const ImmutableOrigin& origin,
               const CoordinationRequest* coordination) {
    const auto& ref = origin.invitation;
    const auto& [environment, project, thread, run] = ref.target;
    const auto operation = coordination ? Scope::sessionCoordinate(environment, thread)
                                        : Scope::consultReply(environment, thread);
    if (person.id != origin.personId || !access.permitsPerson(person, ref.botId, Role::decide) ||
        !access.permitsPersonScope(person, Scope::sessionRead(environment, thread)) ||
        !access.permits(ref.botId, operation, true) ||
        (coordination && coordination->allowDelegation &&
         !access.permits(ref.botId, Scope::workDelegate(environment, thread), true)))
        throw Conflict("Session participation access denied");
}

void redactContext(std::vector<SharedSessionContext>& context, const ImmutableOrigin& origin,
                   const SecretRedactor& redactor) {
    const auto& ref = origin.invitation;
    const auto& target = ref.target;
    redactor.requirePublic({origin.personId, ref.botId, ref.requestId, ref.sourceMessageId,
                            target.environmentId, target.projectId, target.threadId, target.runId});
    for (auto& item : context) {
        redactor.requirePublic({item.itemId});
        auto text = redactor.text(item.text, item.truncated);
        item.truncated = item.truncated || text.size() > maxContextBytes;
        item.text = bounded(text, maxContextBytes);
    }
}

Json contextJson(const std::vector<SharedSessionContext>& context) {
    auto data = Json::array();
    for (const auto& item : context)
        data.push_back({{"itemId", item.itemId}, {"text", item.text}, {"truncated", item.truncated}});
    return data;
}

std::optional<Participation> lookup(sqlite3* db, Id id, std::string_view environment = {},
                                    std::string_view request = {}) {
    Statement row(db, R"SQL(
SELECT c.id, c.environment_id, c.project_id, c.thread_id, c.run_id, c.request_id, c.source_message_id,
       c.bot_id, c.person_id, c.context, p.command_id, a.activation_id, a.expected_epoch, a.goal,
       a.allow_delegation, a.grant_epoch
FROM return_channels c
LEFT JOIN consult_permits p ON p.channel_id=c.id
LEFT JOIN coordination_activations a ON a.channel_id=c.id
WHERE c.id=? OR (c.environment_id=? AND c.request_id=?)
)SQL", {id, environment, request});
    if (!row.next()) return {};
    const ExactRunTarget target{row.string(1), row.string(2), row.string(3), row.string(4)};
    const ImmutableOrigin origin{{target, row.string(5), row.string(6), row.string(7)}, row.string(8)};
    ReturnChannel channel{row.number(0), origin, {}};
    for (const auto& item : row.json(9))
        channel.context.push_back({target, item.at("itemId").get<std::string>(),
                                   item.at("text").get<std::string>(), item.at("truncated").get<bool>()});
    auto command = row.string(10);
    if (!command.empty()) return ConsultPermit{std::move(channel), std::move(command)};
    CoordinationBinding binding{row.string(11), origin, {row.number(12), row.string(13), row.number(14) != 0}};
    const Id epoch = row.number(15);
    std::optional<CoordinationGrant> grant;
    if (epoch) grant.emplace(CoordinationGrant{binding, epoch});
    return CoordinationActivation{std::move(channel), std::move(binding), std::move(grant)};
}

CoordinationActivation loadCoordination(sqlite3* db, Id channel) {
    auto participation = lookup(db, channel);
    if (!participation || !std::holds_alternative<CoordinationActivation>(*participation))
        throw Conflict("Unknown coordination activation");
    return std::get<CoordinationActivation>(std::move(*participation));
}

}

void Store::Impl::initializeParticipationSchema() {
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS return_channels(
 id INTEGER PRIMARY KEY,
 environment_id TEXT NOT NULL, project_id TEXT NOT NULL, thread_id TEXT NOT NULL, run_id TEXT NOT NULL,
 request_id TEXT NOT NULL, source_message_id TEXT NOT NULL, bot_id TEXT NOT NULL, person_id TEXT NOT NULL,
 context TEXT NOT NULL,
 UNIQUE(environment_id,request_id));
)SQL");
    exec("CREATE INDEX IF NOT EXISTS return_channels_thread ON return_channels(environment_id,thread_id)");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS consult_permits(
 channel_id INTEGER PRIMARY KEY REFERENCES return_channels(id),
 command_id TEXT NOT NULL UNIQUE);
)SQL");
    exec(R"SQL(
CREATE TABLE IF NOT EXISTS coordination_activations(
 channel_id INTEGER PRIMARY KEY REFERENCES return_channels(id),
 activation_id TEXT NOT NULL UNIQUE,
 expected_epoch INTEGER NOT NULL CHECK(expected_epoch>=0),
 goal TEXT NOT NULL,
 allow_delegation INTEGER NOT NULL CHECK(allow_delegation IN (0,1)),
 grant_epoch INTEGER NOT NULL DEFAULT 0 CHECK(grant_epoch=0 OR grant_epoch>expected_epoch));
)SQL");
}

Participation ParticipationStore::open(const Person& person, const InvitationRef& ref,
                                       ParticipationAuthority& authority) {
    validateInvitation(ref);
    auto verified = authority.invitation(ref);
    if (!verified || verified->ref != ref || verified->personId != person.id)
        throw Conflict("The Session invitation could not be verified");
    const auto& target = ref.target;
    if (verified->context.size() > maxContextItems) throw Conflict("Too many shared Session context items");
    std::set<std::string_view> sharedItems;
    for (const auto& item : verified->context) {
        if (item.target != target || !validId(item.itemId) || item.text.size() > maxSessionPromptBytes ||
            !sharedItems.insert(item.itemId).second)
            throw Conflict("Invalid shared Session context");
    }
    const auto* request = verified->coordination ? &*verified->coordination : nullptr;
    if (request && (request->expectedEpoch < 0 || trimSessionQuestionText(request->goal).empty() ||
                    request->goal.size() > maxContextBytes))
        throw Conflict("Invalid coordination scope");

    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    const ImmutableOrigin origin{ref, verified->personId};
    authorize(store_.access(), person, origin, request);
    if (db.query("SELECT 1 FROM runs WHERE environment_id=? AND thread_id=? AND run_id=?",
                 {target.environmentId, target.threadId, target.runId}).next())
        throw Conflict("Session participation requires a person-started Run");

    const auto redactor = db.sharedRedactor();
    if (request) redactor.requirePublic({request->goal});
    redactContext(verified->context, origin, redactor);
    if (auto existing = lookup(db.db, 0, target.environmentId, ref.requestId)) {
        const auto* activation = std::get_if<CoordinationActivation>(&*existing);
        auto& channel = std::visit([](auto& item) -> ReturnChannel& { return item.channel; }, *existing);
        if (channel.origin != origin || (activation != nullptr) != (request != nullptr) ||
            (activation && activation->binding.request != *request))
            throw Conflict("The Session invitation has changed");
        const bool sameContext = channel.context == verified->context;
        redactContext(channel.context, channel.origin, redactor);
        if (!sameContext && channel.context != verified->context)
            throw Conflict("The Session invitation has changed");
        tx.commit();
        return std::move(*existing);
    }
    if (request && db.query(R"SQL(
SELECT 1 FROM coordination_activations a JOIN return_channels c ON c.id=a.channel_id
WHERE c.environment_id=? AND c.thread_id=?
)SQL", {target.environmentId, target.threadId}).next())
        throw Conflict("A coordination activation already exists for this Session");

    db.exec(R"SQL(
INSERT INTO return_channels(environment_id,project_id,thread_id,run_id,request_id,source_message_id,bot_id,person_id,context)
VALUES(?,?,?,?,?,?,?,?,?)
)SQL", {target.environmentId, target.projectId, target.threadId, target.runId, ref.requestId,
         ref.sourceMessageId, ref.botId, person.id, contextJson(verified->context)});
    ReturnChannel channel{db.last(), origin, std::move(verified->context)};
    auto id = actionId();
    if (!request) {
        db.exec("INSERT INTO consult_permits(channel_id,command_id) VALUES(?,?)", {channel.id, id});
        tx.commit();
        return ConsultPermit{std::move(channel), std::move(id)};
    }
    db.exec(R"SQL(
INSERT INTO coordination_activations(channel_id,activation_id,expected_epoch,goal,allow_delegation)
VALUES(?,?,?,?,?)
)SQL", {channel.id, id, request->expectedEpoch, request->goal, static_cast<int>(request->allowDelegation)});
    tx.commit();
    return CoordinationActivation{std::move(channel), {std::move(id), origin, *request}, {}};
}

ConsultPermit ParticipationStore::consult(const Person& person, std::string_view bot, Id channel) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db, Transaction::Mode::read);
    auto entry = lookup(db.db, channel);
    auto* permit = entry ? std::get_if<ConsultPermit>(&*entry) : nullptr;
    if (!permit || permit->channel.origin.invitation.botId != bot) throw Conflict("Unknown consultation permit");
    authorize(store_.access(), person, permit->channel.origin, nullptr);
    redactContext(permit->channel.context, permit->channel.origin, db.sharedRedactor());
    tx.commit();
    return std::move(*permit);
}

CoordinationActivation ParticipationStore::coordination(const Person& person, Id channel) const {
    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db, Transaction::Mode::read);
    auto activation = loadCoordination(db.db, channel);
    authorize(store_.access(), person, activation.channel.origin, &activation.binding.request);
    const auto redactor = db.sharedRedactor();
    redactor.requirePublic({activation.binding.request.goal});
    redactContext(activation.channel.context, activation.channel.origin, redactor);
    tx.commit();
    return activation;
}

CoordinationActivation ParticipationStore::observeGrant(const Person& person, Id channel,
                                                       ParticipationAuthority& authority) {
    const auto opening = coordination(person, channel);
    auto grant = authority.grant(opening.binding);
    if (!grant) {
        auto current = coordination(person, channel);
        if (current.acknowledgment) throw Conflict("The current T3 grant could not be verified");
        return current;
    }
    if (grant->binding != opening.binding || grant->epoch <= opening.binding.request.expectedEpoch)
        throw Conflict("T3 returned a different coordination grant");

    auto& db = *store_.p_;
    std::lock_guard lock(db.mutex);
    Transaction tx(db.db);
    auto activation = loadCoordination(db.db, channel);
    authorize(store_.access(), person, activation.channel.origin, &activation.binding.request);
    const auto redactor = db.sharedRedactor();
    redactor.requirePublic({activation.binding.request.goal});
    redactContext(activation.channel.context, activation.channel.origin, redactor);
    if (activation.acknowledgment && *activation.acknowledgment != *grant)
        throw Conflict("A different T3 grant has already been recorded");
    db.exec("UPDATE coordination_activations SET grant_epoch=? WHERE channel_id=? AND grant_epoch=0",
            {grant->epoch, channel});
    activation.acknowledgment = std::move(grant);
    tx.commit();
    return activation;
}

}
