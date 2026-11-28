#include "conversation_messages.hpp"
#include "conversation_storage.hpp"
#include "database.hpp"
#include "web_identity_state.hpp"
#include "flock/policy.hpp"

#include <exception>
#include <optional>
#include <string>

namespace flockv0 {
using conversation::Message;
using conversation::PrincipalSource;

Message ConversationMessages::load(Id id) const {
    auto row = database.query(R"SQL(
SELECT m.reply_to,m.bot_id,m.text,c.scope_key,m.routing,m.person_id,m.person_root,m.principal_source FROM messages m
LEFT JOIN channel_messages c ON c.message_id=m.id WHERE m.id=?
)SQL",
                              {id});
    if (!row.next()) throw Conflict("Conversation message is missing");
    Message message{id,
                    row.number(0),
                    row.string(1),
                    row.string(2),
                    row.string(3),
                    row.json(4),
                    {row.string(5), row.number(6) != 0},
                    static_cast<PrincipalSource>(row.number(7)),
                    std::nullopt};
    if (!message.replyTo || message.replyTo >= id) return message;
    auto anchor = database.query(R"SQL(
SELECT q.id,q.task_id FROM journal target
LEFT JOIN conversation_turns turn ON turn.reply_id=target.id
LEFT JOIN messages original ON original.id=turn.message_id
LEFT JOIN channel_messages source ON source.message_id=original.id
JOIN journal q ON q.id=CASE
  WHEN target.kind='waiting' THEN target.id
  WHEN target.kind='conversation_reply' AND original.bot_id=? AND (?='' OR source.scope_key=?)
    THEN target.task_question_message_id
END
JOIN tasks t ON t.id=q.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE target.id=? AND q.id<=target.id AND q.kind='waiting' AND q.wait_reason='user_question'
  AND t.bot_id=? AND (?='' OR c.scope_key=?)
)SQL",
                                 {message.bot, message.channelScope, message.channelScope, message.replyTo, message.bot,
                                  message.channelScope, message.channelScope});
    if (anchor.next()) message.question = conversation::QuestionAnchor{anchor.number(0), anchor.number(1)};
    return message;
}

Person ConversationMessages::actor(const Message& message) const {
    const auto& admitted = message.admittedActor;
    switch (message.principalSource) {
    case PrincipalSource::trusted: return admitted;
    case PrincipalSource::channel: return {admitted.id, false};
    case PrincipalSource::gitWebhook: return {};
    case PrincipalSource::session: {
        try {
            return sessionActor(admitted.id);
        } catch (const std::exception&) {
            return {};
        }
    }
    case PrincipalSource::web: {
        if (!webIdentity.resolve) return {};
        try {
            if (const auto root = webIdentity.resolve(admitted.id)) return {admitted.id, *root};
        } catch (const std::exception&) {
            return {};
        }
        return {};
    }
    }
    return {};
}

Person ConversationMessages::sessionActor(std::string_view person) const {
    if (webIdentity.resolve) {
        if (const auto root = webIdentity.resolve(person)) return {std::string(person), *root};
    } else if (person == localOwner.id && !permissions.requiresIdentity())
        return localOwner;
    return {};
}
}
