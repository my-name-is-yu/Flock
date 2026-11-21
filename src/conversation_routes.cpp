#include "storage.hpp"
#include "conversation_router.hpp"
#include "conversation_storage.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace flockv0 {
struct conversation::Message {
    Id id, replyTo, sequence, watermark;
    std::string bot, text;
    Json routing;
};

using namespace conversation;
using conversation_storage::hasText;
using conversation_storage::integer;
using conversation_storage::messageSize;
using conversation_storage::verifiedEnvironment;

namespace {
constexpr auto resultSize = 8192;
struct ModelBudgetPaused {};

bool japanese(std::string_view text) {
    return std::any_of(text.begin(),text.end(),[](unsigned char byte) {
        return byte == 0xe3 || (byte >= 0xe4 && byte <= 0xe9);
    });
}

Json sourceRef(const Result& result) {
    Json source = {{"taskId",result.taskId},{"messageId",result.messageId},
                   {"revisionId",result.revision},{"text",bounded(result.text,resultSize)},{"option",result.option}};
    if (!result.provenance.is_null()) source["run"] = result.provenance;
    return source;
}
}

namespace {
template<class... F>
struct Visit : F... { using F::operator()...; };

std::string_view routeName(const Action& action) {
    return std::visit(Visit{
        [](const Respond&) { return "respond"; },
        [](const Status&) { return "status"; },
        [](const CreateTask&) { return "create_task"; },
        [](const AppendInstruction&) { return "append_instruction"; },
        [](const ReviseTask&) { return "revise_task"; },
        [](const AnswerWait&) { return "answer_wait"; },
        [](const CancelTask&) { return "cancel_task"; },
        [](const Clarify&) { return "clarify"; }
    },action);
}

const Target* targetOf(const Action& action) {
    return std::visit([](const auto& item) -> const Target* {
        if constexpr (requires { item.target.taskId; }) return &item.target;
        if constexpr (requires { item.target.has_value(); }) return item.target ? &*item.target : nullptr;
        return nullptr;
    },action);
}

const Result* sourceOf(const Action& action) {
    return std::visit([](const auto& item) -> const Result* {
        if constexpr (requires { item.source; }) return item.source ? &*item.source : nullptr;
        return nullptr;
    },action);
}

const conversation::Task* candidate(const Context& context, Id taskId) {
    const auto found = std::ranges::find(context.tasks,taskId,&conversation::Task::id);
    return found == context.tasks.end() ? nullptr : &*found;
}

bool sameSource(const Result& excerpt, const Result& result) {
    return std::tie(excerpt.taskId,excerpt.messageId,excerpt.revision,excerpt.provenance) ==
           std::tie(result.taskId,result.messageId,result.revision,result.provenance) &&
           result.text.find(excerpt.text) != std::string::npos;
}

bool knownSource(const Context& context, const Result& result) {
    return (context.reply.result && sameSource(result,*context.reply.result)) ||
           (context.pendingClarification && context.pendingClarification->result &&
                sameSource(result,*context.pendingClarification->result)) ||
           (context.recentSource && sameSource(result,*context.recentSource)) ||
           std::ranges::any_of(context.rankedResults,[&](const Result& entry) { return sameSource(result,entry); });
}

bool expiredWait(const Plan& plan, const Context& context) {
    return std::ranges::any_of(plan.routes,[&](const Route& route) {
        if (!std::holds_alternative<AnswerWait>(route.action))
            return false;
        const auto& target = *targetOf(route.action);
        const auto task = candidate(context,target.taskId);
        return !task || task->waitId != target.waitId;
    });
}

Plan routingQuestion(const std::string& text, std::string question) {
    return {{{text,Clarify{std::move(question),{}}}},std::nullopt};
}
}

bool Store::Impl::routeMessage(Id id, Model& model) {
    const auto readMessage = [&]() -> std::optional<Message> {
        auto row = query(R"SQL(
SELECT m.reply_to,m.sequence,m.bot_id,m.text,m.routing,
  (SELECT max(latest.sequence) FROM messages latest WHERE latest.bot_id=m.bot_id)
FROM messages m JOIN journal j ON j.id=m.id
WHERE m.id=? AND j.kind='conversation_message' AND json_extract(j.data,'$.status') IS NULL
AND NOT EXISTS (
  SELECT 1 FROM journal prior WHERE prior.conversation_bot_id=m.bot_id AND prior.id<m.id
  AND prior.kind='conversation_message' AND json_extract(prior.data,'$.status') IS NULL)
)SQL",{id});
        if (!row.next()) return std::nullopt;
        return Message{id,row.number(0),row.number(1),row.number(5),row.string(2),row.string(3),row.json(4)};
    };
    bool waitExpired = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::optional<Message> message;
        Context context;
        {
            std::lock_guard lock(mutex);
            Transaction transaction(db,Transaction::Mode::read);
            message = readMessage();
            if (!message) return true;
            context = routeContext(message->bot,id,message->text,message->replyTo,message->routing);
            transaction.commit();
        }

        const bool ja = japanese(message->text);
        Plan plan;
        if (waitExpired) {
            plan = routingQuestion(message->text,ja ? "その質問はすでに終了しています。現在の質問を確認してください。"
                                                    : "That question has closed. Please check the current question.");
        } else {
            unsigned routeStep = 0;
            const auto decideModel = [&](const ModelInput& input) -> Decision {
                ++routeStep;
                // The message fixes adapter config. Reuse successful calls only for the same prompt.
                Json checkpoint = {{"event",input.event},{"history",input.history},{"tools",input.tools}};
                if (input.event["kind"] == "route_conversation") {
                    auto& generations = checkpoint["taskGenerations"] = Json::array();
                    for (const auto& task : context.tasks) generations.push_back({task.id,task.generation});
                }
                Id modelAttempt = 0;
                {
                    std::lock_guard lock(mutex);
                    Transaction transaction(db);
                    if (auto saved = savedRouteCall(id,routeStep,checkpoint)) {
                        transaction.commit();
                        return std::move(*saved);
                    }
                    const auto budget = messageModelBudget(id,
                        context.reply.kind == Reply::Kind::clarification ? context.reply.id : 0);
                    modelAttempt = reserveModelCall(budget,field(input.event,"kind"),id,0,0);
                    if (!modelAttempt) {
                        exec("INSERT INTO journal(time,kind,data,conversation_bot_id) VALUES(?,'conversation_reply','{}',?)",
                             {unixTime(),message->bot});
                        const auto reply = last();
                        exec("UPDATE journal SET data=? WHERE id=?",
                             {Json{{"messageId",reply},{"replyToMessageId",id},{"route","model_budget"},
                                   {"status","needs_you"},{"text",ja
                                       ? "モデル呼び出しの上限に達しました。続けるには追加の1回を承認してください。"
                                       : "The model call limit is exhausted. Approve one more model call to continue."}},reply});
                        exec("INSERT INTO route_decisions(message_id,reply_id,status,data) VALUES(?,?,'model_budget','{}')",
                             {id,reply});
                        exec("UPDATE journal SET data=json_object('status','needs_you') WHERE id=?",{id});
                    }
                    transaction.commit();
                }
                if (!modelAttempt) throw ModelBudgetPaused{};
                return callModel(model,input,modelAttempt,routeStep,checkpoint);
            };
            try {
                plan = conversation::decide(message->text,context,decideModel,message->routing.at("config"),
                    [&](Id resolvedId) {
                        std::lock_guard lock(mutex);
                        Transaction transaction(db);
                        inheritModelBudget(id,resolvedId);
                        transaction.commit();
                    });
            } catch (const ModelBudgetPaused&) {
                return true;
            } catch (const std::exception&) {
                plan = routingQuestion(message->text,ja ? "このメッセージを判断できませんでした。ここに返信して再試行してください。"
                                                        : "I couldn't route this message. Reply here to try again.");
            }
        }

        std::lock_guard lock(mutex);
        Transaction transaction(db);
        const auto current = readMessage();
        if (!current) return true;
        const auto refreshed = routeContext(current->bot,id,current->text,current->replyTo,current->routing);
        if (current->sequence != message->sequence || current->watermark != message->watermark ||
            refreshed != context) {
            waitExpired |= expiredWait(plan,refreshed);
            continue;
        }
        applyRoutes(*current,refreshed,plan);
        exec("UPDATE model_attempts SET route_input=NULL,route_decision=NULL WHERE message_id=? AND route_decision IS NOT NULL",
             {id});
        transaction.commit();
        return true;
    }
    return false;
}

void Store::Impl::applyRoutes(const Message& message, const Context& context, const Plan& plan) {
    const auto& [id,replyTo,sequence,watermark,bot,text,routing] = message;
    const bool ja = japanese(text);
    const auto& routes = plan.routes;
    Json segments = Json::array(), candidates = Json::array();
    std::set<Id> candidateIds, modificationTargets;
    std::string response, clarificationOperation;
    Json replySource = nullptr;
    std::string status = "done";
    Id linkedTask = 0;
    const auto addLine = [&](std::string_view line) {
        if (line.empty()) return;
        if (!response.empty()) response += '\n';
        response += line;
    };
    const auto* clarification = plan.resolution ? context.clarification(plan.resolution->replyId) : nullptr;
    const bool resolving = clarification && plan.resolution->kind == Resolution::Kind::resolved;
    if (resolving) messageModelBudget(id,clarification->id);
    bool clarify = routes.empty() || (plan.resolution && !clarification);
    for (const auto& route : routes) {
        Json segment = {{"number",segments.size()+1},{"route",routeName(route.action)},{"text",route.segment}};
        if (const auto target = targetOf(route.action)) {
            segment.update({{"taskId",target->taskId},{"generation",target->generation},
                            {"revisionId",target->revision},{"waitId",target->waitId}});
        }
        if (const auto source = sourceOf(route.action)) {
            segment["source"] = sourceRef(*source);
            replySource = segment["source"];
        }
        if (const auto question = std::get_if<Clarify>(&route.action)) {
            clarify = true;
            addLine(question->question);
            candidateIds.insert(question->candidates.begin(),question->candidates.end());
            clarificationOperation = question->operation;
            segment["candidates"] = question->candidates;
            if (question->revisionDraft) {
                const auto& draft = *question->revisionDraft;
                segment["revisionDraft"] = {{"taskId",draft.target.taskId},
                    {"generation",draft.target.generation},{"revisionId",draft.target.revision},
                    {"waitId",draft.target.waitId},{"intent",draft.intent},{"doneWhen",draft.doneWhen}};
            }
        }
        segments.push_back(std::move(segment));
    }
    const auto validate = [&](const Route& route) {
        const auto& action = route.action;
        const bool readOnly = std::holds_alternative<Respond>(action) || std::holds_alternative<Status>(action);
        if (context.reply.kind == Reply::Kind::invalid && !readOnly) return false;
        const auto source = sourceOf(action);
        if (source && !knownSource(context,*source)) return false;
        if (const auto creation = std::get_if<CreateTask>(&action)) {
            if (!hasText(creation->request) || creation->request.size() > messageSize) return false;
            if (field(routing,"environmentId").empty() || field(routing,"projectId").empty()) {
                addLine(ja ? "Bot の実行環境とプロジェクトの設定が必要です。"
                           : "The Bot needs an execution environment and project before it can start work.");
                return false;
            }
            if (source && !terminal(load(source->taskId))) return false;
            if (!creation->environment.empty() && verifiedEnvironment(routing,routing,creation->environment).empty())
                return false;
        }
        const auto target = targetOf(action);
        if (!target) return true;
        const auto view = candidate(context,target->taskId);
        if (!view) return false;
        const auto task = load(target->taskId);
        const auto& wait = task.at("wait");
        if (field(task,"botId") != bot || integer(task,"generation") != target->generation ||
            integer(task,"currentRevision") != target->revision || integer(wait,"id") != target->waitId)
            return false;
        if (std::holds_alternative<Status>(action)) return true;
        if ((std::holds_alternative<AppendInstruction>(action) || std::holds_alternative<ReviseTask>(action)) &&
            !namedTargetMatches(route.segment,context,target->taskId)) return false;
        if (terminal(task) || task["state"] == "cancelling" || !modificationTargets.insert(target->taskId).second)
            return false;
        if (std::holds_alternative<CancelTask>(action) && !explicitCancellation(route.segment)) return false;
        if (source && (source->taskId != target->taskId || source->revision != target->revision))
            return false;
        if (const auto answer = std::get_if<AnswerWait>(&action)) {
            if (wait.value("run",Json(nullptr)) != task.value("activeRun",Json(nullptr)) ||
                integer(wait,"eventId") != integer(task,"eventId")) return false;
            if (!hasText(answer->answer) || answer->answer.size() > 8000 || field(wait,"reason") != "user_question")
                return false;
        }
        if (std::holds_alternative<AnswerWait>(action)) {
            const bool anchored = replyTo && context.reply.kind == Reply::Kind::wait &&
                                  context.reply.taskId == target->taskId && context.reply.waitId == target->waitId;
            if (task["state"] != "waiting" || !target->waitId ||
                (!anchored && (!view->waitAtIntake || context.staleWaits || context.moreWaits))) return false;
            const auto originalWait = std::pair{target->taskId,target->waitId};
            if (resolving && (clarification->incompleteWaitsAtIntake ||
                std::ranges::find(clarification->waitsAtIntake,originalWait) == clarification->waitsAtIntake.end()))
                return false;

        }
        if (const auto revision = std::get_if<ReviseTask>(&action)) {
            if (!revision->environment.empty() && verifiedEnvironment(task,routing,revision->environment).empty()) {
                addLine(ja ? "指定された実行環境を Bot の設定から特定できません。"
                           : "I couldn't match that execution environment to this Bot's configured destinations.");
                return false;
            }
            for (const auto& value : {revision->intent,revision->doneWhen}) {
                const bool verbatim = route.segment.find(value) != std::string::npos ||
                                      (resolving && text.find(value) != std::string::npos);
                if (!value.empty() && (!hasText(value) || value.size() > 8000 || !verbatim))
                    return false;
            }
        }
        return true;
    };
    if (!clarify) {
        for (const auto& route : routes) {
            if (validate(route)) continue;
            clarify = true;
            if (const auto target = targetOf(route.action)) candidateIds.insert(target->taskId);
            break;
        }
    }

    const auto saveInstruction = [&](const Target& target, std::string_view instruction,
                                     const std::optional<Result>& source, const ReviseTask* change) {
        auto task = load(target.taskId);
        auto revision = target.revision;
        bool conflicting = change != nullptr;
        std::string environment;
        if (change) {
            const auto oldIntent = field(task,"intent").empty() ? field(task,"request") : field(task,"intent");
            const auto oldDone = field(task,"doneWhen");
            const auto intent = change->intent.empty() ? oldIntent : change->intent;
            const auto done = change->doneWhen.empty() ? oldDone : change->doneWhen;
            if (!oldDone.empty() && (intent != oldIntent || done != oldDone)) revision = addRevision(task,intent,done,id);
            else if (oldDone.empty()) {
                task["intent"] = intent;
                if (!change->doneWhen.empty()) {
                    task["requestedDoneWhen"] = change->doneWhen;
                    task["criteriaSourceMessageId"] = id;
                }
            }
            environment = verifiedEnvironment(task,routing,change->environment);
            conflicting = !change->doneWhen.empty() || !change->intent.empty() || environment.empty() ||
                          environment != field(task,"environmentId") || task.value("replanning",false);
        }
        Json pending = {{"messageId",id},{"text",instruction},{"revisionId",revision},{"conflicting",conflicting}};
        if (source) {
            pending["source"] = sourceRef(*source);
            auto& references = task["sourceResultRefs"];
            if (references.is_null()) references = Json::array();
            if (std::ranges::find(references,pending["source"]) == references.end())
                references.push_back(pending["source"]);
        }
        if (!environment.empty()) {
            pending["environmentId"] = environment;
            if (conflicting) task["pendingEnvironmentId"] = environment;
        }
        if (conflicting) {
            pending["replaceKey"] = !environment.empty() ? "environment" : !change->doneWhen.empty() ? "criteria"
                                    : !change->intent.empty() ? "intent" : "target";
            task["replanning"] = true;
        }
        if (!task.contains("pendingInstructions")) task["pendingInstructions"] = Json::array();
        task["pendingInstructions"].push_back(pending);
        journal(target.taskId,"instruction_accepted",pending);
        save(task);
        linkedTask = target.taskId;
        status = conflicting ? "stopping" : "working";
        addLine(conflicting
            ? (ja ? "変更を保存しました。前の実行が終了したことを確認してから切り替えます。"
                  : "I saved the change. I’ll switch once the previous work has stopped.")
            : (ja ? "補足を保存しました。実行側に送れる段階で引き継ぎます。"
                  : "I saved the instruction. I’ll pass it on when the current work reaches a safe point."));
    };

    if (!clarify) {
        for (std::size_t index = 0; index < routes.size(); ++index) {
            std::visit(Visit{
                [&](const Respond& item) { addLine(item.text); },
                [&](const Status& item) { addLine(item.text); },
                [&](const CreateTask& item) {
                    const auto environment = item.environment.empty() ? field(routing,"environmentId")
                        : verifiedEnvironment(routing,routing,item.environment);
                    linkedTask = submit({bot,item.request,environment,field(routing,"projectId"),
                                         routing.at("config"),{},0},id,item.source ? sourceRef(*item.source) : Json(nullptr));
                    if (item.source && item.source->state == "cancelled") {
                        const auto prior = load(item.source->taskId);
                        auto task = load(linkedTask);
                        if (const auto criteria = prior.value("requestedDoneWhen",field(prior,"doneWhen")); !criteria.empty())
                            task["requestedDoneWhen"] = criteria;
                        task["restartContext"] = {{"intent",prior.value("intent",prior.at("request"))},
                                                  {"taskId",item.source->taskId},
                                                  {"pendingInstructions",prior.value("cancelledInstructions",Json::array())}};
                        auto rows = query(R"SQL(
SELECT entry.value FROM outbox, json_each(outbox.data,'$.instructions') entry
WHERE outbox.task_id=? AND outbox.phase='accepted'
ORDER BY outbox.rowid DESC, entry.key DESC LIMIT 20
)SQL",{item.source->taskId});
                        auto earlier = Json::array();
                        std::size_t remaining = 8000;
                        while (remaining && rows.next()) {
                            auto instruction = rows.json(0);
                            instruction["text"] = bounded(field(instruction,"text"),remaining);
                            remaining -= field(instruction,"text").size();
                            earlier.push_back(std::move(instruction));
                        }
                        std::reverse(earlier.begin(),earlier.end());
                        auto& instructions = task["restartContext"]["pendingInstructions"];
                        instructions.insert(instructions.begin(),earlier.begin(),earlier.end());
                        save(task);
                    }
                    segments[index]["effectTaskId"] = linkedTask;
                    status = "working";
                    addLine(item.source ? (ja ? "元の作業を参照して、新しい作業として受け付けました。"
                                               : "I queued a new task linked to the earlier work.")
                                        : (ja ? "作業を受け付けました。" : "I queued the task."));
                },
                [&](const AppendInstruction& item) { saveInstruction(item.target,item.instruction,item.source,nullptr); },
                [&](const ReviseTask& item) { saveInstruction(item.target,item.instruction,item.source,&item); },
                [&](const AnswerWait& item) {
                    operate(item.target.taskId,item.target.waitId,"answer",{{"text",item.answer}},true);
                    linkedTask = item.target.taskId;
                    status = "working";
                    addLine(ja ? "質問への回答を受け付けました。" : "I saved your answer to the question.");
                },
                [&](const CancelTask& item) {
                    operate(item.target.taskId,0,"cancel",Json::object(),true);
                    linkedTask = item.target.taskId;
                    status = load(linkedTask)["state"] == "cancelled" ? "done" : "stopping";
                    addLine(ja ? "停止を受け付けました。" : "I accepted the stop request.");
                },
                [](const Clarify&) {}
            },routes[index].action);
        }
    } else {
        status = "needs_you";
        for (const auto& route : routes) {
            const auto target = targetOf(route.action);
            if (target && !std::holds_alternative<Status>(route.action)) candidateIds.insert(target->taskId);
            if (const auto source = sourceOf(route.action); source &&
                !std::holds_alternative<Respond>(route.action) && !std::holds_alternative<Clarify>(route.action))
                candidateIds.insert(source->taskId);
        }
        for (const auto& task : context.tasks) {
            if (candidateIds.contains(task.id) && task.state != "completed" && task.state != "cancelled")
                candidates.push_back({{"taskId",task.id},{"request",task.name}});
        }
        if (response.empty()) addLine(ja ? "対象の作業と指示を確認してください。"
                                         : "Please confirm the task and instruction.");
    }
    const auto* question = routes.size() == 1 ? std::get_if<Clarify>(&routes.front().action) : nullptr;
    const bool resultQuestion = question && question->source && knownSource(context,*question->source);
    if (clarification && (!clarify || (resultQuestion && clarification->result &&
                                        sameSource(*question->source,*clarification->result)))) {
        exec("UPDATE route_decisions SET status=?,resolution_id=? WHERE reply_id=? AND status='awaiting_resolution'",
             {clarify || plan.resolution->kind == Resolution::Kind::superseded ? "superseded" : "resolved",
              id,plan.resolution->replyId});
    }
    if (response.empty()) addLine(ja ? "確認しました。" : "I’ve read your message.");

    const std::string_view route = clarify ? "clarify" : routes.size() == 1 ? routeName(routes.front().action) : "compound";
    const auto replyStatus = clarify ? "awaiting_resolution" : "applied";
    exec("INSERT INTO journal(task_id,time,kind,data,conversation_bot_id) VALUES(?,?,'conversation_reply','{}',?)",
         {linkedTask ? Json(linkedTask) : Json(nullptr),unixTime(),bot});
    const auto reply = last();
    Json answer = {{"messageId",reply},{"replyToMessageId",id},{"text",response},{"route",route},{"status",status}};
    if (!candidates.empty()) answer["candidates"] = candidates;
    if (clarificationOperation == "destination") answer["destinations"] = context.environments;
    if (!replySource.is_null() && (!clarify || resultQuestion)) answer["source"] = replySource;
    exec("UPDATE journal SET data=? WHERE id=?", {answer,reply});
    exec("UPDATE journal SET task_id=?,data=? WHERE id=?", {linkedTask ? Json(linkedTask) : Json(nullptr),
        Json{{"status",status}},id});
    exec("INSERT INTO route_decisions(message_id,reply_id,status,data) VALUES(?,?,?,?)",
         {id,reply,replyStatus,Json{{"sequence",sequence},{"inboxWatermark",watermark},
                                    {"segments",segments},{"candidates",candidates},{"operation",clarificationOperation}}});
    for (const auto& candidate : candidates)
        exec("INSERT INTO route_candidates(message_id,task_id) VALUES(?,?)",{id,candidate.at("taskId")});
}
}
