#include "runtime_task_decisions.hpp"
#include "task_interaction_storage.hpp"
#include "runtime_outbox_dispatch.hpp"
#include "runtime_run_reports.hpp"
#include "runtime_task_state.hpp"
#include "runtime_task_updates.hpp"
#include "flock/types.hpp"
#include "flock/json.hpp"
#include "flock/session_question.hpp"
#include "flock/command.hpp"
#include "flock/task.hpp"
#include "flock/task_interaction.hpp"
#include "flock/wait.hpp"
#include "conversation_turns.hpp"
#include "journal_conversation.hpp"
#include "task_events.hpp"
#include "task_records.hpp"
#include "wait_storage.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace flockv0 {
runtime::TaskDecisions::TaskDecisions(ConversationTurns& conversationTurns, TaskEvents& taskEvents, TaskRecords& taskRecords,
                                      WaitStorage& waitStorage, TaskUpdates& updates, OutboxDispatch& dispatch)
    : conversationTurns(conversationTurns), taskEvents(taskEvents), taskRecords(taskRecords), waitStorage(waitStorage), updates(updates),
      dispatch(dispatch) {}

void runtime::TaskDecisions::applyLaunchDefaults(TaskData& task, const Json& defaults) {
    auto session = task.config.value("session", Json::object());
    if (!session.is_object()) session = Json::object();
    for (const auto* key : {"model", "providerInstanceId", "workspaceStrategy"}) {
        if (defaults.contains(key))
            session[key] = defaults.at(key);
        else
            session.erase(key);
    }
    if (session.empty())
        task.config.erase("session");
    else
        task.config["session"] = std::move(session);
}

bool runtime::TaskDecisions::continueFreshSession(const TaskData& task, const Json* launchDefaults) {
    if (task.projectId.empty() || !task.freshContextRequested || !RunReports::hasSettledRun(task) || task.pendingInstructions.empty())
        return false;
    return updates.change(task, [&](TaskData& current) {
        if (!current.actionId.empty()) return false;
        auto event = taskEvents.currentEvent(current);
        if (!event.is_null() && event.at("consumed") != true && (event.at("kind") != "result" || event.contains("userAnswer")))
            return false;
        if (event.is_null()) event = Json::object();
        const auto tool = current.newSessionRequired ? CommandTool::delegate : CommandTool::follow_up;
        const TaskDecision decision{
            {std::string(commandToolName(tool)), Json::object(), "Apply accepted user instructions after the assigned Run settled"}};
        const auto action = actionId();
        if (event.contains("id")) updates.recordDecision(current, event, decision, action);
        updates.consumeEvent(current);
        if (launchDefaults && tool == CommandTool::delegate) applyLaunchDefaults(current, *launchDefaults);
        return dispatch.queueAction(current, action, tool, "Continue this Flock task.", decision.args, event);
    });
}

void runtime::TaskDecisions::launch(const TaskData& task, const Json& event, const Json* launchDefaults) {
    updates.change(task, [&](TaskData& current) {
        if (!current.actionId.empty() || current.activeRun) return false;
        // The Session prompt already carries the task intent, so the instruction only points at it.
        constexpr auto instruction = "Do the work described in the task intent.";
        Json args{{"instruction", instruction}};
        if (current.criteriaProposal) args["doneWhen"] = current.criteriaProposal->doneWhen;
        const TaskDecision decision{{"delegate", args, "Start the requested work"}};
        updates.establishCriteria(current, args);
        if (launchDefaults) applyLaunchDefaults(current, *launchDefaults);
        updates.consumeEvent(current);
        const auto action = actionId();
        updates.recordDecision(current, event, decision, action);
        if (current.runTargets().empty()) return dispatch.queueAction(current, action, CommandTool::delegate, instruction, args, event);
        dispatch.queueDestinations(current, action, instruction, args, event);
        return true;
    });
}

bool runtime::TaskDecisions::answerSessionQuestion(const TaskData& task, const Json& event) {
    if (!task.activeRun || !task.question || task.question->fields.size() != 1) return false;
    const auto& question = *task.question;
    const auto& asked = question.fields.front();
    // The person picks from the labels Flock showed, and the Session expects the option's value.
    auto answer = event.at("userAnswer").get<std::string>();
    if (std::ranges::find(asked.options, answer, &QuestionOption::value) == asked.options.end())
        if (const auto shown = std::ranges::find(asked.options, answer, &QuestionOption::label); shown != asked.options.end())
            answer = shown->value;
    const Json args{{"requestId", question.target.requestId}, {"answers", sessionQuestionAnswers(question.fields, {{asked.id, answer}})}};
    return updates.change(task, [&](TaskData& current) {
        if (!current.actionId.empty() || !current.question || current.question->target.requestId != question.target.requestId) return false;
        updates.consumeEvent(current);
        const auto action = actionId();
        updates.recordDecision(current, event, {{"answer_session_question", args, "Send the person's answer"}}, action);
        return dispatch.queueAction(current, action, CommandTool::answer_session_question, "", args, event);
    });
}

void runtime::TaskDecisions::handle(const TaskData& task, const Json& event, const Json* launchDefaults) {
    updates.change(task, [&](TaskData& current) {
        const bool idle = current.actionId.empty() && !current.activeRun;
        const auto& kind = event.at("kind");
        if (kind == "policy_outcome" && !event.contains("userAnswer")) {
            updates.consumeEvent(current);
            settleUndecided(current, "denied", taskRecords, waitStorage, field(event, "text"));
            return true;
        }
        // A reply to a stopped task is the person's next instruction. The Session prompt carries its text.
        const bool answered = event.contains("userAnswer") && (!current.projectId.empty() || current.selectedSession);
        if (idle && (!current.pendingInstructions.empty() || answered)) {
            const auto tool =
                RunReports::hasSettledRun(current) && !current.newSessionRequired ? CommandTool::follow_up : CommandTool::delegate;
            const TaskDecision decision{{std::string(commandToolName(tool)), Json::object(), "Send the person's new instructions"}};
            const auto action = actionId();
            updates.recordDecision(current, event, decision, action);
            updates.consumeEvent(current);
            if (launchDefaults && tool == CommandTool::delegate) applyLaunchDefaults(current, *launchDefaults);
            return dispatch.queueAction(current, action, tool, "Continue this Flock task.", decision.args, event);
        }
        if (kind == "question" && current.question && current.question->fields.size() == 1 && !event.contains("userAnswer")) {
            // Show the Session's own question to the person. Their answer goes back to the Session.
            const auto& first = current.question->fields.front();
            std::vector<std::string> choices;
            for (const auto& option : first.options) choices.push_back(option.label);
            const auto text = !first.prompt.empty() ? first.prompt : !first.header.empty() ? first.header : "The Session needs an answer.";
            updates.consumeEvent(current);
            updates.recordDecision(current, event, {{"ask_user", {{"question", text}}, "Ask the person the Session's question"}},
                                   actionId());
            const auto waitId =
                waitStorage.wait(current, ModelQuestionWait{std::move(choices), {}, current.question->target.requestId}, text);
            current.question->target = questionTarget(current, waitId);
            return true;
        }
        updates.consumeEvent(current);
        // Flock relays one field. Several fields, or an answer the Session refused, are answered in T3.
        if (kind == "question" && current.question && current.activeRun) {
            waitStorage.wait(current, SimpleWaitReason::t3_input, "Answer the Session's question in T3.");
            return true;
        }
        const auto status = field(event, "status");
        const auto reason = kind == "result"           ? (status == "completed"     ? "finished"
                                                          : status == "interrupted" ? "interrupted"
                                                                                    : "failed")
                            : kind == "question"       ? "waiting_for_person"
                            : kind == "external_check" ? "check_due"
                                                       : "unknown";
        if (!current.sourceMessageId) {
            if (idle || (reason == std::string_view("unknown") && !current.activeRun))
                settleUndecided(current, reason, taskRecords, waitStorage, field(event, "text"));
            else if (kind == "question")
                waitStorage.wait(current, SimpleWaitReason::t3_input, "Answer the Session's question in T3.");
            return true;
        }
        const auto& run = current.activeRun ? current.activeRun : current.lastRun;
        conversationTurns.enqueueTaskWake(
            *current.sourceMessageId,
            {run ? run->environmentId : current.environmentId, run ? run->ref.threadId : std::string{}, "", reason, 0, current.id},
            event.at("id").get<Id>());
        return true;
    });
}
}
