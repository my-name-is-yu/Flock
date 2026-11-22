#include "demo.hpp"

#include <algorithm>

namespace flockv0::app {
namespace {
class DemoModel final : public Model {
    Decision decide(const ModelInput& in) override {
        const auto event = in.event.value("kind","");
        if (event == "conversation_turn") {
            const auto& turn = in.event.at("turn");
            if (!turn.empty()) {
                const auto& last = turn.back();
                const bool ok = last.at("result").value("ok",false);
                const auto tool = last.value("tool","");
                std::string text = !ok ? "The demo could not apply that request."
                    : tool == "answer_task_question" ? "I passed your answer to the task."
                    : tool == "read_result" ? "The demo result records successful completion of simulated work. "
                                              "It contains no implementation details."
                    : "I queued your request.";
                return {"reply",{{"text",text}},"The fixed demo reports what it did."};
            }
            const auto& reply = in.event.at("replyTo");
            if (reply.is_object() && reply.value("kind","") == "task_question" && reply.value("current",false)) {
                return {"answer_task_question",{{"taskId",reply["taskId"]},{"waitId",reply["waitId"]},
                    {"text",in.event.value("text","")}},"The user replied to the demo question."};
            }
            if (reply.is_object() && reply.value("kind","") == "result")
                return {"read_result",{{"messageId",reply["messageId"]}},"The user asked about a demo result."};
            // A reply to the Bot's explanation of a result asks about that same result.
            if (reply.is_object() && reply.value("kind","") == "bot_message") {
                const auto source = reply.value("inReplyTo",Json::object()).value("replyToMessageId",Json(nullptr));
                if (!source.is_null())
                    return {"read_result",{{"messageId",source}},"The user followed up on an explained demo result."};
            }
            return {"start_task",{{"request",in.event.value("text","")}},"Fixed demo intake."};
        }
        if (event == "result" && std::ranges::find(in.tools,"complete") != in.tools.end()) {
            return {"complete",{{"report",in.event.value("report","")}},
                "The demo Session reported that it completed the requested work."};
        }
        if (std::ranges::find(in.tools,"delegate") != in.tools.end()) {
            std::string answer;
            for (const auto& row : in.history)
                if (row.value("kind","") == "user_answer") answer = row.value("data",Json::object()).value("text","");
            const auto request = in.task.value("request","");
            if (answer.empty() && request.starts_with("ask:")) {
                auto question = request.substr(4);
                if (question.find_first_not_of(" \t\r\n") == std::string::npos) question = "What should the demo Session focus on?";
                return {"ask_user",{{"question",question}},"The demo request calls for a user answer before work begins."};
            }
            Json args = {{"instruction",request + (answer.empty() ? "" : "\nUser answer: " + answer)}};
            if (in.task.value("doneWhen","").empty()) args["doneWhen"] = "The demo Session returns a completion report.";
            return {"delegate",std::move(args),"A demo Session can handle this request."};
        }
        throw ModelError("No fixed demo decision for this event",false);
    }
};

class DemoT3 final : public T3 {
    std::string authGeneration(const std::string&, const Json&) override { return "fixed-demo"; }

    RunRef run(const Command& command) {
        const auto thread = command.tool == "follow_up" ? command.target.threadId : "demo-thread-" + command.actionId;
        return {thread,"demo-run-" + command.actionId,""};
    }

    Delivery send(const Command& command) override {
        return {Delivery::Kind::Accepted,run(command),""};
    }

    Delivery reconcile(const Command& command) override {
        const auto reference = run(command);
        // The demo Run is a pure function of the previously persisted action.
        return {Delivery::Kind::Accepted,reference,""};
    }

    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        Observation observed;
        observed.kind = Observation::Kind::Finished;
        observed.status = "completed";
        observed.report = "The fixed demo Session completed its assigned work.";
        observed.resultRunId = run.runId;
        observed.settled = true;
        return observed;
    }

    void interrupt(const std::string&, const Json&, const RunRef&) override {}
};
} // namespace

std::unique_ptr<Model> makeDemoModel() { return std::make_unique<DemoModel>(); }
std::unique_ptr<T3> makeDemoT3() { return std::make_unique<DemoT3>(); }
} // namespace flockv0::app
