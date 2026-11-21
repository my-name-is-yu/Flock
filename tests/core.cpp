#include "flock/core.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

using namespace flockv0;
namespace {
void check(bool b, std::string_view message) { if (!b) throw std::runtime_error(std::string(message)); }

struct FixedModel : Model {
    int calls = 0;
    Decision decide(const ModelInput& input) override {
        ++calls;
        if (input.event["kind"] == "intake") return {"delegate",{{"instruction","Check main"},{"doneWhen","Provide a report on main"}},"The request specifies main"};
        if (input.event["kind"] == "question")
            return {"answer_session_question",{{"requestId","q1"},{"answers",{{"branch","main"}}}},"The request specifies main"};
        check(input.task["doneWhen"] == "Provide a report on main","lost doneWhen");
        check(input.event["report"] == "main confirmed","lost report");
        return {"complete",{{"report","main confirmed"}},"The Run report satisfies the saved criterion"};
    }
};
struct FixedT3 : T3 {
    int launches = 0, replies = 0, polls = 0;
    Observation current;
    std::string authGeneration(const std::string&,const Json&) override { return "fixed-auth"; }
    Delivery send(const Command& cmd) override {
        if (cmd.tool == "delegate") {
            ++launches;
            check(cmd.instruction.find(cmd.actionId) != std::string::npos,"missing action marker");
            return {Delivery::Kind::Accepted,{"thread-1","run-1","t3://thread-1"},{}};
        }
        check(cmd.tool == "answer_session_question" && cmd.questionId == "q1" && cmd.target.runId == "run-1","wrong reply target");
        ++replies;
        current = {};
        return {Delivery::Kind::Accepted,{},{}};
    }
    Delivery reconcile(const Command&) override { throw std::runtime_error("unexpected reconciliation"); }
    Observation observe(const std::string&,const Json&,const RunRef& ref) override {
        ++polls;
        check(ref.runId == "run-1","wrong run");
        return current;
    }
    void interrupt(const std::string&,const Json&,const RunRef&) override { throw std::runtime_error("unexpected interrupt"); }
};
}
int main() {
    char temp[] = "/tmp/flock-core-XXXXXX";
    const auto path = std::filesystem::path(mkdtemp(temp));
    try {
        const auto db = path / "state.db";
        FixedModel model;
        FixedT3 t3;
        auto notes = [](std::string_view) { return BotNotes{"Check the branch",""}; };
        Id id;
        {
            Store store(db);
            Runtime runtime(store,model,t3,notes);
            id = store.submit({"release","Check main","env","proj"});
            for (int i = 0; i < 3; ++i) runtime.tick();
            check(store.task(id)["state"] == "running","delegate did not start");
            for (int i = 0; i < 3; ++i) runtime.tick();
            check(model.calls == 1 && t3.polls == 3,"idle monitoring called model");
            t3.current.kind = Observation::Kind::Question;
            t3.current.question = {"q1",Json::array({{{"id","branch"},{"required",true}}})};
            for (int i = 0; i < 3; ++i) runtime.tick();
            check(t3.replies == 1 && store.task(id)["state"] == "running","did not return to same Run");
        }
        {
            Store store(db);
            Runtime runtime(store,model,t3,notes);
            t3.current.kind = Observation::Kind::Finished;
            t3.current.status = "completed";
            t3.current.report = "main confirmed";
            t3.current.settled = true;
            runtime.tick();
            runtime.tick();
            runtime.tick();
            const auto task = store.task(id);
            check(task["state"] == "completed" && task["runs"].size() == 1,"restart did not complete");
            check(task["additionalUsed"] == 0 && t3.launches == 1 && t3.replies == 1 && model.calls == 3,
                  "unexpected Run or extra inference");
        }
        std::filesystem::remove_all(path);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove_all(path);
        return 1;
    }
}
