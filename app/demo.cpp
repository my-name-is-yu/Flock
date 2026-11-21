#include "demo.hpp"

#include <algorithm>

namespace flockv0::app {
namespace {
class DemoModel final : public Model {
    Decision decide(const ModelInput& in) override {
        const auto event = in.event.value("kind","");
        if (event == "route_conversation") {
            Json route = {{"kind","create_task"}};
            const auto& reply = in.event.at("reply");
            if (reply["kind"] == "wait") {
                for (const auto& task : in.event.at("tasks")) {
                    if (task["taskId"] == reply["taskId"] && task["wait"]["reason"] == "user_question")
                        route = {{"kind","answer_wait"},{"taskId",reply["taskId"]}};
                }
            } else if (reply["kind"] == "result") {
                route = {{"kind","answer_result"},{"resultMessageId",reply["resultMessageId"]}};
            } else if (reply["kind"] == "clarification") {
                route = {{"kind","clarify"},{"text","Use a direct request or reply to a demo question."}};
            }
            return {"route_conversation",{{"routes",Json::array({route})}},"Fixed demo routing."};
        }
        if (event == "answer_conversation_result") {
            return {"answer_conversation_result",
                {{"text","The demo result records successful completion of simulated work. It contains no implementation details."}},
                "The fixed demo reports only completion."};
        }
        if (event == "result" && std::ranges::find(in.tools,"complete") != in.tools.end()) {
            return {"complete",{{"report",in.event.value("report","")}},
                "The demo Session reported that it completed the requested work."};
        }
        if (event == "bot_request" && in.event.value("initiator","") == "user") {
            return {"reply_to_bot",{{"kind","result"},{"text","I received your message. This is a simulated Bot reply."}},
                "The fixed demo confirms receipt in the Bot conversation."};
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
