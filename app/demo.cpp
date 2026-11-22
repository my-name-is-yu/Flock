#include "demo.hpp"

#include <algorithm>

namespace flockv0::app {
namespace {
class DemoModel final : public Model {
public:
    explicit DemoModel(std::string question) : question_(std::move(question)) {}

private:
    std::string question_;
    Decision decide(const ModelInput& in) override {
        const auto event = in.event.value("kind","");
        if (event == "conversation_turn") {
            const auto& turn = in.event.at("turn");
            if (!turn.empty()) {
                const auto& last = turn.back();
                const auto& result = last.at("result");
                const auto tool = last.value("tool","");
                const char* text = "I queued your request.";
                if (!result.value("ok",false)) text = "The demo could not apply that request.";
                else if (tool == "answer_task_question") text = "I passed your answer to the task.";
                else if (tool == "read_result") {
                    text = result.value("kind","") == "cancelled" ? "The demo request was stopped."
                        : "The demo result records successful completion of simulated work. "
                          "It contains no implementation details.";
                }
                return {"reply",{{"text",text}},"The fixed demo reports what it did."};
            }
            const auto& reply = in.event.at("replyTo");
            const auto text = in.event.value("text","");
            const auto kind = reply.is_object() ? reply.value("kind","") : "";
            if (kind == "task_question" && reply.value("current",false)) {
                return {"answer_task_question",{{"taskId",reply["taskId"]},{"waitId",reply["waitId"]},
                    {"text",text}},"The user replied to the open task question."};
            }
            if (kind == "result" && text == "Explain this result")
                return {"read_result",{{"messageId",reply["messageId"]}},"The user requested a demo result explanation."};
            if ((kind.empty() || kind == "bot_message") && text == "Run a demo task")
                return {"start_task",{{"request",text}},"The user selected the demo task."};
            return {"reply",{{"text","This is the fixed Flock demo."},{"suggestions",Json::array({"Run a demo task"})}},
                "Offer the explicit demo action."};
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
            if (answer.empty() && !question_.empty() && std::ranges::find(in.tools,"ask_user") != in.tools.end())
                return {"ask_user",{{"question",question_}},"The demo was configured to request an answer."};
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

std::unique_ptr<Model> makeDemoModel(std::string question) { return std::make_unique<DemoModel>(std::move(question)); }
std::unique_ptr<T3> makeDemoT3() { return std::make_unique<DemoT3>(); }
} // namespace flockv0::app
