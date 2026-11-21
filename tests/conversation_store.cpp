#include "flock/core.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <map>
#include <thread>
#include <unistd.h>

using namespace flockv0;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    std::filesystem::path dir;

    Fixture() {
        char pattern[] = "/tmp/flock-conversation-XXXXXX";
        if (const auto* path = mkdtemp(pattern)) dir = path;
        else throw std::runtime_error("Cannot create fixture");
    }
    ~Fixture() { std::filesystem::remove_all(dir); }

    std::filesystem::path database() const { return dir / "state.db"; }
    static Intake bot(std::string id = "alpha", std::string project = "project") {
        return {std::move(id),"","env",std::move(project),Json::object(),{},0};
    }
};

Id task(Store& store, std::string name, std::string bot = "alpha", std::string project = "project") {
    auto intake = Fixture::bot(std::move(bot),std::move(project));
    intake.request = std::move(name);
    return store.submit(intake);
}

Json reply(Store& store, Id message, std::string_view bot = "alpha") {
    const auto timeline = store.conversation(bot,0,100);
    for (const auto& row : timeline)
        if (row["kind"] == "conversation_reply" && row["data"]["replyToMessageId"] == message)
            return row["data"];
    throw std::runtime_error("Missing conversation reply");
}

Id receive(Store& store, std::string_view id, std::string_view text, Id anchor = 0) {
    return store.receive(Fixture::bot(),id,text,anchor).at("messageId");
}

Decision routing(Json routes, std::string_view resolution = {}) {
    Json args = {{"routes",std::move(routes)}};
    if (!resolution.empty()) args["resolution"] = resolution;
    return {"route_conversation",std::move(args),{}};
}

Decision route(const char* kind, Json fields = Json::object()) {
    fields["kind"] = kind;
    return routing(Json::array({fields}));
}

struct ScriptedModel : Model {
    std::function<Decision(const ModelInput&)> conversation;

    Decision decide(const ModelInput& input) override {
        if (input.event["kind"] == "route_conversation" || input.event["kind"] == "answer_conversation_result")
            return conversation(input);
        if (input.event.at("kind") == "intake")
            return {"delegate",{{"instruction","Run the fixture work"},{"doneWhen","Provide a fixture report"}},"Run the fixture"};
        if (input.event.at("kind") == "question") {
            if (input.event.contains("userAnswer")) {
                return {"answer_session_question",{{"requestId",input.event.at("question").at("requestId")},
                        {"answers",{{"name",input.event.at("userAnswer")}}}},"Use the requested name"};
            }
            return {"ask_user",{{"question","何という名前にしますか？"}},"The name is required"};
        }
        check(input.event.at("kind") == "result","Unexpected execution fixture event");
        return {"complete",{{"report",input.event.at("report")}},"The fixture report exists"};
    }
};

struct SimulatedT3 : T3 {
    std::map<std::string,Observation> observations;
    std::vector<Command> answers;
    unsigned launches = 0;

    std::string authGeneration(const std::string&, const Json&) override { return "fixture-auth"; }
    Delivery send(const Command& command) override {
        if (command.tool == "answer_session_question") {
            answers.push_back(command);
            observations[command.target.runId] = {};
            return {Delivery::Kind::Accepted,{},{}};
        }
        check(command.tool == "delegate","Unexpected work command in the Store fixture");
        const auto suffix = std::to_string(++launches);
        return {Delivery::Kind::Accepted,{"thread-" + suffix,"run-" + suffix,{}},{}};
    }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&, const Json&, const RunRef& run) override {
        return observations[run.runId];
    }
    void interrupt(const std::string&, const Json&, const RunRef&) override {}
};

struct RuntimeFixture {
    ScriptedModel model;
    SimulatedT3 t3;
    Runtime runtime;

    explicit RuntimeFixture(Store& store) : runtime(store,model,t3,[](auto) { return BotNotes{}; }) {}

    void reach(Store& store, Id id, std::string_view state) {
        for (int i = 0; i < 16 && store.task(id).at("state") != state; ++i) runtime.tick();
        check(store.task(id).at("state") == state,"The work did not reach the required fixture state");
    }

    void question(Store& store, Id id, std::string request) {
        reach(store,id,"running");
        const auto run = store.task(id).at("activeRun").at("runId").get<std::string>();
        t3.observations[run] = {.kind = Observation::Kind::Question,
            .question = {std::move(request),Json::array({{{"id","name"},{"required",true}}})}};
        reach(store,id,"waiting");
    }

    void finish(Store& store, Id id, std::string result) {
        reach(store,id,"running");
        const auto run = store.task(id).at("activeRun").at("runId").get<std::string>();
        t3.observations[run] = {.kind = Observation::Kind::Finished, .status = "completed", .report = std::move(result),
            .resultRunId = run, .itemId = "report-" + run, .settled = true};
        reach(store,id,"completed");
    }
};

void checkSingleDelivery(Store& store, Id message) {
    const auto tasks = store.tasks("alpha");
    check(tasks.size() == 1 && tasks.at(0).at("request") == "Write a release note" &&
          tasks.at(0).at("state") == "queued","Delivery did not queue exactly the requested work");
    const auto timeline = store.conversation("alpha",0,100);
    const auto messages = std::ranges::count_if(timeline,[](const Json& row) {
        return row.at("kind") == "conversation_message";
    });
    const auto userMessage = std::ranges::find_if(timeline,[&](const Json& row) {
        return row.at("kind") == "conversation_message" && row.at("id") == message;
    });
    check(messages == 1 && userMessage != timeline.end() &&
          userMessage->at("data").at("text") == "Write a release note",
          "Delivery lost or duplicated the user's message");
    const auto replies = std::ranges::count_if(timeline,[](const Json& row) {
        return row.at("kind") == "conversation_reply";
    });
    check(replies == 1 && reply(store,message).at("route") == "create_task",
          "Delivery lost or duplicated the task acknowledgement");
}

void retryAcrossConnections() {
    Fixture fixture;
    Store left(fixture.database()), right(fixture.database());
    Id a = 0, b = 0;
    std::exception_ptr first, second;
    std::thread one([&] {
        try { a = receive(left,"retry","Write a release note"); }
        catch (...) { first = std::current_exception(); }
    });
    std::thread two([&] {
        try { b = receive(right,"retry","Write a release note"); }
        catch (...) { second = std::current_exception(); }
    });
    one.join();
    two.join();
    if (first) std::rethrow_exception(first);
    if (second) std::rethrow_exception(second);
    check(a && a == b,"Concurrent delivery returned different receipts");
    check(left.tasks("alpha").empty(),"Receipt intake executed work before routing");
    Store restarted(fixture.database());
    RuntimeFixture execution(restarted);
    unsigned calls = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        check(input.event.at("text") == "Write a release note","Restart lost the pending message");
        ++calls;
        return route("create_task");
    };
    execution.runtime.routeConversation();
    checkSingleDelivery(restarted,a);
    bool rejected = false;
    try { receive(right,"retry","Write a different release note"); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"Changed message text did not conflict");
    rejected = false;
    try { receive(right,"retry","Write a release note",reply(restarted,a).at("messageId")); }
    catch (const Conflict&) { rejected = true; }
    check(rejected,"Changed message reply target did not conflict");
    check(receive(restarted,"retry","Write a release note") == a,"Receipt was lost on restart");
    execution.runtime.routeConversation();
    checkSingleDelivery(restarted,a);
    check(calls == 1,"The retry rerouted an applied message");
}

void clarificationAndStatus() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto a = task(store,"リリース作業");
    const auto b = task(store,"計測作業");
    const auto other = task(store,"別 Bot の作業","beta");
    execution.model.conversation = [&](const ModelInput& input) {
        const auto text = input.event.at("text").get<std::string>();
        if (text == "それ止めて")
            return route("clarify",{{"text","どの作業を停止しますか？"},{"candidates",{a,b}}});
        if (text == "今どこまでできた？" || text == "進捗どうなってる") return route("status");
        if (text == "計測作業の方です")
            return routing(Json::array({{{"kind","cancel_task"},{"taskId",b}}}),"resolved");
        check(text == "計測作業を再開して","Unexpected clarification fixture input");
        const auto& results = input.event.at("results");
        const auto source = std::ranges::find_if(results,[&](const Json& result) {
            return result.at("taskId") == b && result.at("state") == "cancelled";
        });
        check(source != results.end(),"The cancelled source was missing");
        return route("create_task",{{"resultMessageId",source->at("messageId")},{"resume",true}});
    };
    const auto id = receive(store,"stop","それ止めて");
    execution.runtime.routeConversation();
    const auto clarifier = reply(store,id);
    const auto hasCandidate = [&](Id id, std::string_view name) {
        return std::ranges::any_of(clarifier.at("candidates"),[&](const Json& candidate) {
            return candidate.at("taskId") == id && candidate.at("request") == name;
        });
    };
    check(clarifier.at("route") == "clarify" && clarifier.at("candidates").size() == 2 &&
          hasCandidate(a,"リリース作業") && hasCandidate(b,"計測作業"),
          "Ambiguous stop did not name exactly this Bot's candidates");
    check(store.task(a)["state"] == "queued" && store.task(b)["state"] == "queued","An ambiguous stop changed a task");
    const auto statusId = receive(store,"status","進捗どうなってる");
    execution.runtime.routeConversation();
    const auto status = reply(store,statusId);
    const auto& progress = status.at("text").get_ref<const std::string&>();
    check(status.at("route") == "status" && progress.find("リリース作業") != std::string::npos &&
          progress.find("計測作業") != std::string::npos && progress.find("順番待ち") != std::string::npos &&
          progress.find("別 Bot の作業") == std::string::npos,
          "Status did not report this Bot's queued work while clarification was open");
    check(store.tasks("alpha").size() == 2 && store.task(a).at("state") == "queued" &&
          store.task(b).at("state") == "queued" && store.task(a).at("runs").empty() &&
          store.task(b).at("runs").empty() && execution.t3.launches == 0,"A status request changed the work");
    const auto paraphrase = receive(store,"paraphrased-status","今どこまでできた？");
    execution.runtime.routeConversation();
    check(reply(store,paraphrase).at("route") == "status" && store.tasks("alpha").size() == 2 &&
          execution.t3.launches == 0,"A model-routed progress paraphrase changed the work");
    receive(store,"select","計測作業の方です");
    execution.runtime.routeConversation();
    check(store.task(a)["state"] == "queued" && store.task(b)["state"] == "cancelled" &&
          store.task(other)["state"] == "queued" && store.clarifications("alpha").empty(),"Selection did not affect exactly the chosen task");
    const auto resumeId = receive(store,"resume","計測作業を再開して");
    execution.runtime.routeConversation();
    const auto resume = reply(store,resumeId);
    const auto tasks = store.tasks("alpha");
    const auto resumed = std::ranges::find_if(tasks,[&](const Json& task) {
        return task.at("id") != a && task.at("id") != b;
    });
    check(resume.at("route") == "create_task" && tasks.size() == 3 && resumed != tasks.end() &&
          resumed->at("sourceTaskId") == b && resumed->at("state") == "queued" &&
          store.task(b).at("state") == "cancelled","Explicit resume did not queue new work linked to the cancelled task");
}

Json pendingText(Store& store, Id id) {
    auto text = Json::array();
    for (const auto& instruction : store.task(id).value("pendingInstructions",Json::array()))
        text.push_back(instruction.at("text"));
    return text;
}

void compoundAndUnmatchedCancellation() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto a = task(store,"A");
    const auto b = task(store,"B");
    const auto separate = task(store,"Release","beta");
    const auto extra = task(store,"Inventory","beta");
    execution.model.conversation = [&](const ModelInput& input) {
        const auto text = input.event.at("text").get<std::string>();
        if (text == "A は main で。別件、C を実装して") {
            return routing(Json::array({
                {{"kind","append_instruction"},{"segment","A は main で"},{"taskId",a}},
                {{"kind","create_task"},{"segment","C を実装して"}}
            }));
        }
        if (text == "main で。別件、D を実装して") {
            return routing(Json::array({
                {{"kind","clarify"},{"segment","main で"},{"text","どの作業を指していますか？"},{"candidates",{a,b}}},
                {{"kind","create_task"},{"segment","D を実装して"}}
            }));
        }
        if (text == "存在しない作業を止めて") {
            return route("cancel_task",{{"taskId",a}});
        }
        check(text == "それ止めて","A foreign reply reached the model");
        return route("clarify",{{"text","対象の作業はどれですか？"},{"candidates",{separate,extra}}});
    };
    const auto compoundId = receive(store,"compound","A は main で。別件、C を実装して");
    execution.runtime.routeConversation();
    const auto compound = reply(store,compoundId);
    const auto tasks = store.tasks("alpha");
    const auto created = std::ranges::find_if(tasks,[](const Json& task) {
        return task.at("request") == "C を実装して";
    });
    check(compound.at("route") == "compound" && tasks.size() == 3 && created != tasks.end(),
          "The independent segment did not create exactly the requested task");
    const auto c = created->at("id").get<Id>();
    const auto checkWork = [&](const char* message) {
        check(store.tasks("alpha").size() == 3 && store.task(a).at("state") == "queued" &&
              store.task(b).at("state") == "queued" && store.task(c).at("state") == "queued" &&
              pendingText(store,a) == Json::array({"A は main で"}) &&
              pendingText(store,b).empty() && pendingText(store,c).empty() && execution.t3.launches == 0,message);
    };
    checkWork("The compound message did not save only the intended instruction");

    const auto unclearId = receive(store,"unclear","main で。別件、D を実装して");
    execution.runtime.routeConversation();
    const auto unclear = reply(store,unclearId);
    check(unclear.at("route") == "clarify","An unclear compound request did not ask which work to change");
    checkWork("A compound request applied a segment while its other segment was unclear");

    const auto response = store.receive(Fixture::bot("beta"),"unknown","存在しない作業を止めて");
    execution.runtime.routeConversation();
    check(reply(store,response.at("messageId"),"beta").at("route") == "clarify" &&
          store.task(separate).at("state") == "queued","An unmatched name cancelled unrelated work");

    const auto stop = store.receive(Fixture::bot("beta"),"stop","それ止めて");
    execution.runtime.routeConversation();
    const auto betaQuestion = reply(store,stop.at("messageId"),"beta");
    check(betaQuestion.at("route") == "clarify" && betaQuestion.at("candidates").size() == 2,
          "The other Bot did not issue an actionable clarification");
    const auto crossBot = receive(store,"cross","A は main で。別件、E を実装して",betaQuestion.at("messageId"));
    execution.runtime.routeConversation();
    reply(store,crossBot);
    checkWork("A reply to another Bot's clarification applied work");
    check(store.task(separate).at("state") == "queued" && store.task(extra).at("state") == "queued" &&
          store.clarifications("beta").size() == 2,"A foreign reply changed the other Bot's work or clarification");
}

void independentWorkAndSupplement() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto login = task(store,"ログイン画面を実装して");
    execution.reach(store,login,"running");
    const auto original = store.task(login);
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& text = input.event.at("text");
        if (text == "CSV形式で売上レポートを作って") return route("create_task");
        check(text == "補足として表をCSV形式で出して","Unexpected continuation fixture input");
        return route("append_instruction",{{"taskId",login}});
    };
    const auto independent = receive(writer,"new-report","CSV形式で売上レポートを作って");
    execution.runtime.routeConversation();
    const auto tasks = store.tasks("alpha");
    check(reply(store,independent).at("route") == "create_task" && tasks.size() == 2 &&
          tasks[0].at("request") == "CSV形式で売上レポートを作って" &&
          tasks[0].at("blockedBy").at("id") == login && store.task(login) == original,
          "Independent work changed the login Task or bypassed the project FIFO");
    const auto supplement = receive(writer,"login-table","補足として表をCSV形式で出して");
    execution.runtime.routeConversation();
    const auto updated = store.task(login);
    check(reply(store,supplement).at("route") == "append_instruction" && store.tasks("alpha").size() == 2 &&
          pendingText(store,login) == Json::array({"補足として表をCSV形式で出して"}) &&
          updated.at("runs") == original.at("runs") && updated.at("revisions") == original.at("revisions") &&
          updated.at("remaining") == original.at("remaining"),"A supplement created work or changed the acceptance revision or budget");
    const auto explicitNew = task(writer,"補足として表をCSV形式で出して");
    check(explicitNew != login && store.tasks("alpha").size() == 3 &&
          store.task(explicitNew).at("request") == "補足として表をCSV形式で出して" && pendingText(store,explicitNew).empty(),
          "Explicit submit routed an instruction into an existing Task");
}

void naturalWaitAnswer() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto open = task(writer,"名称を設定して");
    execution.question(store,open,"name-question");
    const auto original = store.task(open);
    const auto foreign = task(writer,"別 Bot の名前の質問","beta","foreign");
    execution.question(store,foreign,"foreign-name");
    const auto foreignBefore = store.task(foreign);
    unsigned routeCalls = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        ++routeCalls;
        const auto& tasks = input.event.at("tasks");
        const auto found = std::ranges::find_if(tasks,[&](const Json& task) { return task.at("taskId") == open; });
        check(found != tasks.end() && found->at("wait").at("currentAtIntake") == true &&
              found->at("wait").at("text") == "何という名前にしますか？",
              "Available wait did not accompany the message sent in natural language");
        const auto& text = input.event.at("text");
        if (text == "Flockです。それと対象の作業を止めて") {
            return routing(Json::array({
                {{"kind","answer_wait"},{"segment","Flockです"},{"taskId",open}},
                {{"kind","clarify"},{"segment","それと対象の作業を止めて"},{"text","どの作業を停止しますか？"}}
            }));
        }
        check(text == "Flockです" && input.event.at("reply").at("kind") == "none","Explicit reply was needed for natural answer");
        return route("answer_wait",{{"taskId",open}});
    };
    const auto compound = receive(writer,"unclear-wait","Flockです。それと対象の作業を止めて");
    execution.runtime.routeConversation();
    check(reply(store,compound).at("route") == "clarify" && store.task(open) == original &&
          execution.t3.answers.empty(),"A compound clarification applied its first wait answer");

    const auto crossBot = receive(writer,"foreign-wait","別の名前です",foreignBefore.at("wait").at("messageId"));
    execution.runtime.routeConversation();
    check(reply(store,crossBot).at("route") == "respond" && store.task(open) == original &&
          store.task(foreign) == foreignBefore && routeCalls == 1,
          "Explicit reply to another Bot's wait was routed or changed current work");

    const auto answer = receive(writer,"name","Flockです");
    execution.runtime.routeConversation();
    check(reply(store,answer).at("route") == "answer_wait" && store.task(open).at("wait").is_null() &&
          store.tasks("alpha").size() == 1 && store.task(open).at("runs") == original.at("runs"),
          "The natural-language answer created work or missed the current wait");
    execution.reach(store,open,"running");
    check(execution.t3.answers.size() == 1 && execution.t3.answers[0].questionId == "name-question" &&
          execution.t3.answers[0].target.runId == original.at("runs")[0].at("runId").get<std::string>() &&
          execution.t3.answers[0].answers == Json{{"name","Flockです"}},"The wait answer did not reach its original simulated Run");

    const auto replacement = task(writer,"後続の質問","alpha","next-question");
    execution.question(store,replacement,"later-name");
    const auto replacementBefore = store.task(replacement);
    const auto stale = receive(writer,"late-name","違う名前です",original.at("wait").at("messageId"));
    execution.runtime.routeConversation();
    check(reply(store,stale).at("route") == "respond" && store.task(replacement) == replacementBefore &&
          store.task(foreign) == foreignBefore && routeCalls == 2 && execution.t3.answers.size() == 1,
          "A stale wait reply was rerouted to a different wait");
}

void clarificationKeepsTheUserInstruction() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    constexpr auto initial = "作業をお願いします";
    constexpr auto instruction = "CSV形式で売上レポートを作って";
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& text = input.event.at("text");
        if (text == initial) return route("clarify",{{"text","どの作業を始めますか？"}});
        check(text == instruction && input.event.at("reply").at("originalText") == initial,
              "The clarified instruction was routed without its conversation context");
        return routing(Json::array({{{"kind","create_task"}}}),"resolved");
    };
    const auto first = receive(writer,"vague-request",initial);
    execution.runtime.routeConversation();
    const auto clarification = reply(store,first);
    check(clarification.at("route") == "clarify" && store.tasks("alpha").empty() &&
          store.clarifications("alpha").size() == 1,"An incomplete request queued work before clarification");
    const auto resolved = receive(writer,"clarified-request",instruction,clarification.at("messageId"));
    execution.runtime.routeConversation();
    const auto tasks = store.tasks("alpha");
    check(reply(store,resolved).at("route") == "create_task" && store.clarifications("alpha").empty() &&
          tasks.size() == 1 && tasks.front().at("request").get<std::string>().find(instruction) != std::string::npos &&
          tasks.front().at("runs").empty() && execution.t3.launches == 0,
          "Resolving the clarification discarded the user's actionable instruction");
}

void implicitClarificationRequiresAnEligibleReply() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    constexpr auto original = "Set up automation for the project";
    constexpr auto answer = "Use the repository's hosting service";
    constexpr auto independent = "Write release notes";
    constexpr auto another = "Prepare the deployment";
    constexpr auto question = "Which hosting service should I use?";
    Id clarificationId = 0;
    const auto resolve = [](Id anchor) {
        auto decision = routing(Json::array({{{"kind","create_task"}}}),"resolved");
        if (anchor) decision.args["clarificationMessageId"] = anchor;
        return decision;
    };
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& event = input.event;
        if (event.at("text") == original || event.at("text") == another)
            return route("clarify",{{"text",question}});
        if (event.at("text") == independent) return route("create_task");
        check(event.at("text") == answer,"Unexpected clarification fixture input");
        if (event.at("reply").at("kind") == "clarification") return resolve(0);
        return resolve(clarificationId);
    };
    const auto send = [&](std::string_view key, std::string_view text, Id anchor = 0) {
        const auto id = receive(writer,key,text,anchor);
        execution.runtime.routeConversation();
        return reply(store,id);
    };
    clarificationId = send("original",original).at("messageId");
    check(send("independent",independent).at("route") == "create_task" &&
          store.tasks("alpha").front().at("request") == independent && store.clarifications("alpha").size() == 1,
          "An independent request inherited or closed the pending clarification");
    check(send("answer",answer).at("route") == "create_task","An unanchored answer did not resolve the clarification");
    const auto tasks = store.tasks("alpha");
    check(store.clarifications("alpha").empty() && tasks.size() == 2 &&
          tasks.front().at("request") == std::string(original) + "\n\nUser clarification:\n" + answer &&
          tasks.front().at("runs").empty(),"Implicit resolution lost the original request or answer");

    const auto second = send("second",original).at("messageId").get<Id>();
    const auto third = send("third",another).at("messageId").get<Id>();
    clarificationId = second;
    check(send("ambiguous",answer).at("route") == "clarify" && store.tasks("alpha") == tasks,
          "An unanchored answer chose between competing clarifications");
    if (store.clarifications("alpha").size() > 2) {
        const auto extra = store.clarifications("alpha").front().at("messageId").get<Id>();
        send("dismiss-ambiguous","dismiss this clarification",extra);
    }
    check(send("explicit",answer,third).at("route") == "create_task" &&
          store.tasks("alpha").front().at("request") == std::string(another) + "\n\nUser clarification:\n" + answer &&
          store.clarifications("alpha").size() == 1 &&
          store.clarifications("alpha").front().at("messageId") == second,
          "An explicit reply discarded the original request or closed a different clarification");
    const auto afterExplicit = store.tasks("alpha");
    clarificationId = third;
    check(send("stale",answer).at("route") == "clarify" && store.tasks("alpha") == afterExplicit &&
          std::ranges::any_of(store.clarifications("alpha"),[&](const Json& item) {
              return item.at("messageId") == second;
          }),"A model-supplied stale clarification ID changed work or closed a different question");
    check(execution.t3.launches == 0,"Clarification routing started a Run");
}

void explicitFactsReplaceResultClarificationAtomically() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto taskId = task(writer,"Compare results");
    execution.finish(store,taskId,"1. Skip verification\n2. Verify the March build");
    const auto result = store.latestTaskResult(taskId).at("id").get<Id>();
    int interpreted = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& event = input.event;
        if (input.tools.front() == "route_conversation") {
            if (event.at("text") == "Explain option 2 using the March evidence")
                return route("clarify",{{"text","Which March evidence should I use?"},
                    {"resultMessageId",result},{"resultOption",2}});
            if (event.at("text") != "yes" && event.at("text") != "no")
                return route("answer_result",{{"resultMessageId",result},{"resultOption",2}});
            auto plan = route("answer_result");
            plan.args["resolution"] = "resolved";
            return plan;
        }
        check(event.at("source").at("messageId") == result &&
              event.at("source").at("text") == "2. Verify the March build","the answer lost its exact source");
        const auto inputText = event.at("text").get<std::string>();
        ++interpreted;
        if (interpreted == 1)
            return Decision{"clarify_conversation_result",{{"text","Do you have a March test run?"}},{}};
        if (interpreted == 2) {
            check(inputText.find("Do you have a March test run?") != std::string::npos && inputText.ends_with("yes"),
                  "the explicit affirmative fact never reached the answer model");
            return Decision{"clarify_conversation_result",{{"text","Did it use a different workload?"}},{}};
        }
        check(inputText.find("Did it use a different workload?") != std::string::npos && inputText.ends_with("no"),
              "the explicit negative fact did not reach the newest clarification");
        return Decision{"answer_conversation_result",{{"text","The March run can serve as evidence for the same workload."}},{}};
    };
    const auto send = [&](std::string_view key,std::string_view body,Id anchor) {
        const auto message = receive(writer,key,body,anchor);
        execution.runtime.routeConversation();
        return reply(store,message);
    };
    const auto question = send("first","Explain option 2",result);
    check(question["route"] == "clarify" && store.clarifications("alpha").size() == 1,
          "the initial result clarification was not saved");
    const auto next = send("affirmative","yes",question.at("messageId"));
    const auto pending = store.clarifications("alpha");
    check(next["route"] == "clarify" && pending.size() == 1 && pending.front()["messageId"] == next["messageId"] &&
          pending.front()["source"]["messageId"] == result,"a follow-up left the previous clarification open");
    const auto resolved = send("negative","no",next.at("messageId"));
    check(resolved["route"] == "respond" && store.clarifications("alpha").empty() &&
          interpreted == 3 && store.tasks("alpha").size() == 1,"the result clarification applied a work action or retained a hold");
    const auto selected = send("selected-clarification","Explain option 2 using the March evidence",result);
    check(selected.at("route") == "clarify" && selected.at("text") == "Which March evidence should I use?" &&
          selected.at("source") == question.at("source") && interpreted == 3,
          "the routing clarification lost the selected excerpt or called the answer model");
}

void followUpsKeepThePriorResult() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto original = task(writer,"Original report");
    execution.finish(store,original,"1. Ignore samples\n" + std::string(3800,'x') + "\n2. Use paired observations");
    const auto sourceId = store.latestTaskResult(original).at("id").get<Id>();
    const auto unrelated = task(writer,"Unrelated report");
    execution.finish(store,unrelated,"1. Compare earlier dates\n2. Discard the baseline");
    const auto unrelatedId = store.latestTaskResult(unrelated).at("id").get<Id>();
    execution.model.conversation = [&](const ModelInput& input) {
        if (input.tools.front() == "answer_conversation_result") {
            check(input.event.at("source").at("text") == "2. Use paired observations","the selected old report was truncated");
            return Decision{"answer_conversation_result",{{"text","Pair the observations from the two runs."}},{}};
        }
        const auto& text = input.event.at("text");
        if (text == "Explain option 2") return route("answer_result",{{"resultMessageId",sourceId},{"resultOption",2}});
        if (text == "案2以外を実装") return route("create_task",{{"resultMessageId",sourceId},{"resultOption",2}});
        if (text == "案2を適用して") return route("create_task",{{"resultMessageId",unrelatedId},{"resultOption",2}});
        return route("create_task");
    };
    const auto send = [&](std::string_view key,std::string_view body,Id anchor = 0) {
        const auto message = receive(writer,key,body,anchor);
        execution.runtime.routeConversation();
        return reply(store,message);
    };
    const auto answer = send("explain","Explain option 2",sourceId);
    check(answer["route"] == "respond" && answer["source"]["option"] == 2,"the Bot answer dropped its selection");
    const auto selected = send("selected","案2を実装");
    check(selected["route"] == "create_task" && selected["source"]["messageId"] == sourceId &&
          store.tasks("alpha").front()["sourceResultRefs"][0]["text"] == "2. Use paired observations",
          "the follow-up ignored the previous answer's report");
    send("explain-again","Explain option 2",sourceId);
    const auto deictic = send("deictic","それを実装して");
    check(deictic["route"] == "create_task" && deictic["source"]["messageId"] == sourceId,
          "the deictic follow-up lost the cited result");
    const auto count = store.tasks("alpha").size();
    check(send("exclude","案2以外を実装",sourceId)["route"] == "clarify" &&
          send("ambiguous","案2を適用して")["route"] == "clarify" && store.tasks("alpha").size() == count,
          "an excluded or unrelated option created work");
    check(send("count","改善提案2件をまとめて")["route"] == "create_task" && store.tasks("alpha").size() == count + 1,
          "a proposal count was treated as a selected option");
}

void comparisonsDoNotSelectOneOfSeveralOptions() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto taskId = task(writer,"Compare the alternatives");
    const std::string report = "1. Keep the current flow\n2. Compare both options";
    execution.finish(store,taskId,report);
    const auto resultId = store.latestTaskResult(taskId).at("id").get<Id>();
    const auto tasksBefore = store.tasks("alpha");
    const auto launches = execution.t3.launches;
    constexpr auto comparison = "Compare option 1 and option 2.";
    constexpr auto comparisonWithRest = "Compare option 2 with the rest.";
    constexpr auto mutation = "Create a task from option 1 and option 2.";
    int answered = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& text = input.event.at("text").get_ref<const std::string&>();
        if (input.tools.front() == "answer_conversation_result") {
            check((text == comparison || text == comparisonWithRest) &&
                  input.event.at("source").at("messageId") == resultId &&
                  input.event.at("source").at("text") == report,
                  "the comparison answer did not receive the whole referenced result");
            ++answered;
            return Decision{"answer_conversation_result",{{"text","The options differ in their validation steps."}},{}};
        }
        if (text == comparison || text == comparisonWithRest)
            return route("answer_result",{{"resultMessageId",resultId}});
        return route("create_task",{{"resultMessageId",resultId}});
    };
    for (const auto request : {comparison,comparisonWithRest}) {
        const auto compareId = receive(writer,request,request,resultId);
        execution.runtime.routeConversation();
        const auto response = reply(store,compareId);
        check(response.at("route") == "respond" && response.at("source").at("messageId") == resultId &&
              response.at("source").at("option") == 0 && store.tasks("alpha") == tasksBefore &&
              execution.t3.launches == launches,"a comparison selected an option or dispatched work");
    }
    check(answered == 2,"the comparison did not call the answer model for both phrasings");
    const auto mutationId = receive(writer,"mutate-options",mutation,resultId);
    execution.runtime.routeConversation();
    check(reply(store,mutationId).at("route") == "clarify" && store.tasks("alpha") == tasksBefore &&
          execution.t3.launches == launches,
          "a mutating request selected between multiple referenced options");
}

void excerptRepliesExpandToWholeResult() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    const auto taskId = task(writer,"Report with options");
    const std::string report = "1. Keep the current flow\n2. Compare both options";
    execution.finish(store,taskId,report);
    const auto resultId = store.latestTaskResult(taskId).at("id").get<Id>();
    int answered = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& text = input.event.at("text").get_ref<const std::string&>();
        if (input.tools.front() == "answer_conversation_result") {
            ++answered;
            if (answered == 1)
                check(input.event.at("source").at("option") == 1 &&
                      input.event.at("source").at("text") == "1. Keep the current flow",
                      "the selected excerpt did not reach the answer model");
            else
                check(input.event.at("source").value("option",0) == 0 &&
                      input.event.at("source").at("text") == report,
                      "the excerpt follow-up did not receive the whole result");
            return Decision{"answer_conversation_result",{{"text","Here is the answer."}},{}};
        }
        if (text == "Explain option 1")
            return route("answer_result",{{"resultMessageId",resultId},{"resultOption",1}});
        if (text == "Explain option 2" || text == "Compare option 2 with the rest.")
            return route("answer_result",{{"resultMessageId",resultId}});
        if (text == "Tell me about option 2")
            return route("answer_result",{{"resultMessageId",resultId},{"resultOption",1}});
        return route("create_task");
    };
    const auto firstId = receive(writer,"first","Explain option 1",resultId);
    execution.runtime.routeConversation();
    const auto first = reply(store,firstId);
    check(first.at("route") == "respond" && first.at("source").at("option") == 1,
          "the first answer did not select option 1");
    const auto secondId = receive(writer,"second","Explain option 2",first.at("messageId"));
    execution.runtime.routeConversation();
    const auto second = reply(store,secondId);
    check(second.at("route") == "respond" && second.at("source").at("option") == 0 &&
          second.at("source").at("text") == report && answered == 2,
          "the omitted option kept the replied excerpt");
    const auto compareId = receive(writer,"compare","Compare option 2 with the rest.",first.at("messageId"));
    execution.runtime.routeConversation();
    check(reply(store,compareId).at("route") == "respond" && answered == 3,
          "the comparison to an excerpt did not reach the answer model");
    const auto conflictId = receive(writer,"conflict","Tell me about option 2",first.at("messageId"));
    execution.runtime.routeConversation();
    check(reply(store,conflictId).at("route") == "clarify" && answered == 3,
          "a conflicting option was answered instead of rejected");
}

void longResultOptionsStayAvailable() {
    for (const auto paddingSize : {3000, 3500}) {
        Fixture fixture;
        Store store(fixture.database()), writer(fixture.database());
        RuntimeFixture execution(store);
        const auto id = task(writer,"Compare the proposals");
        const std::string option = "## 2. Pair measurements\n"
            "Compare both builds using exactly the same workload.\n"
            "Repeat the paired run to separate variance from regressions.";
        execution.finish(store,id,"## 1. Compare one build\n" + std::string(paddingSize,'A') +
            "\n\n" + option + "\n\n## 3. Defer comparison\n\n## 3. Drop the comparison");
        const auto history = store.history(id,0,100);
        const auto report = std::ranges::find_if(history,[](const Json& row) { return row.at("kind") == "report"; });
        check(report != history.end(),"Missing report in the long result fixture");
        const auto sourceId = report->at("id").get<Id>();
        const auto before = store.tasks("alpha");
        const auto launches = execution.t3.launches;
        std::string userQuestion;
        int answers = 0;
        int selectedOption = 2;
        execution.model.conversation = [&](const ModelInput& input) {
            const auto& event = input.event;
            if (input.tools.front() == "route_conversation") {
                const auto& results = event.at("results");
                const auto candidate = std::ranges::find_if(results,[&](const Json& result) {
                    return result.at("messageId") == sourceId;
                });
                check(candidate != results.end() && candidate->at("options") == Json::array({
                    {{"option",1},{"text",bounded("## 1. Compare one build\n" + std::string(paddingSize,'A'),512)}},
                    {{"option",2},{"text",option}}}),
                    "the router did not offer the bounded options in source order");
                return route("answer_result",{{"resultMessageId",sourceId},{"resultOption",selectedOption}});
            }
            check(event.at("text") == userQuestion && event.at("source").at("messageId") == sourceId &&
                  event.at("source").at("text") == option,
                  "The answer model did not receive the exact option and question");
            ++answers;
            return Decision{"answer_conversation_result",{{"text","同じ負荷で両ビルドを比較する案です。"}},{}};
        };
        for (const auto anchor : {sourceId,Id{0}}) {
            for (const auto request : {"2案目を説明して","後者をもう少し詳しく","両方を同じ条件で測るほうを具体的に"}) {
                userQuestion = request;
                const auto message = receive(writer,std::to_string(anchor) + request,request,anchor);
                execution.runtime.routeConversation();
                const auto response = reply(store,message);
                check(response.at("route") == "respond" && response.at("source").at("text") == option &&
                      response.at("text") == "同じ負荷で両ビルドを比較する案です。",
                      "Reading a long result did not answer with the complete, selected option");
            }
        }
        for (const auto number : {3,4}) {
            selectedOption = number;
            userQuestion = std::to_string(number) + "案目を要約して";
            const auto message = receive(writer,"unavailable-option-" + std::to_string(number),userQuestion);
            execution.runtime.routeConversation();
            check(reply(store,message).at("route") == "clarify","A missing or duplicated option was answered");
        }
        check(answers == 6 && store.tasks("alpha") == before && execution.t3.launches == launches,
              "Reading the result changed work or failed to call the answer model");
    }
}

void resultTargetMustComeFromTheCandidatesOrReply() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto first = task(store,"Build evaluation");
    const auto second = task(store,"Release process");
    const auto foreign = task(store,"Other Bot's evaluation","beta");
    execution.finish(store,first,"1. Measure each build in isolation\n2. Compare paired measurements");
    execution.finish(store,second,"1. Deploy in stages\n2. Deploy in one batch");
    execution.finish(store,foreign,"1. Inspect logs\n2. Inspect traces");
    const Id source = store.latestTaskResult(first).at("id");
    const Id other = store.latestTaskResult(second).at("id");
    const Id hidden = store.latestTaskResult(foreign).at("id");
    const auto before = store.tasks();
    const auto launches = execution.t3.launches;
    constexpr auto unexpectedTool = "この案について説明して";

    Id proposed = source;
    int answers = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& event = input.event;
        if (input.tools.front() == "answer_conversation_result") {
            check(event.at("source").at("messageId") == source &&
                  event.at("source").at("text") == "2. Compare paired measurements",
                  "the reply did not use the selected result and option");
            ++answers;
            if (event.at("text") == unexpectedTool)
                return Decision{"create_task",Json::object(),{}};
            return Decision{"answer_conversation_result",{{"text","ビルドごとの計測を同じ条件で対にして比較します。"}},{}};
        }
        const auto& results = event.at("results");
        const auto offers = [&](Id id) {
            return std::ranges::any_of(results,[&](const Json& result) { return result.at("messageId") == id; });
        };
        check(offers(source) && offers(other) && !offers(hidden),"the routing context included the wrong Bot's result");
        if (event.at("text") == "後者をもう少し詳しく")
            return route("clarify",{{"text","ビルド評価とリリース手順のどちらですか？"}});
        return route("answer_result",{{"resultMessageId",proposed},{"resultOption",2}});
    };
    const auto send = [&](std::string_view key,std::string_view text,Id anchor = 0) {
        const auto message = receive(store,key,text,anchor);
        execution.runtime.routeConversation();
        return reply(store,message);
    };
    const auto ambiguous = send("ambiguous","後者をもう少し詳しく");
    check(ambiguous.at("route") == "clarify" && ambiguous.at("text") == "ビルド評価とリリース手順のどちらですか？" &&
          !ambiguous.contains("source") && answers == 0,"an ambiguous report was answered");
    const auto named = send("described","ビルドを比較する方の後者をもう少し詳しく");
    const auto anchored = send("anchored","2案目を説明して",source);
    check(named.at("route") == "respond" && anchored.at("route") == "respond" &&
          named.at("source") == anchored.at("source") && anchored.at("source").at("option") == 2 && answers == 2,
          "the paraphrase and the result reply did not select the same option");

    check(send("unexpected-tool",unexpectedTool,source).at("route") == "clarify",
          "the explanation step accepted a work action");
    proposed = other;
    const auto wrongReply = send("wrong-reply","こちらの案を説明して",anchored.at("messageId"));
    check(wrongReply.at("route") == "clarify" && wrongReply.at("source") == anchored.at("source"),
          "the proposed result overrode an explicit reply");
    proposed = hidden;
    check(send("foreign","その案を説明して").at("route") == "clarify",
          "the proposed result escaped the Bot's candidate set");
    check(answers == 3 && store.tasks() == before && execution.t3.launches == launches,
          "explaining or rejecting a result reference changed work");
}

void clarificationCannotTransferAnExpiredAnswer() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    constexpr auto originalText = "Flockです";
    constexpr auto secondName = "帳票名を決める";
    const auto first = task(writer,"製品名を決める","alpha","product");
    const auto second = task(writer,secondName,"alpha","report");
    execution.question(store,first,"product-name");
    check(store.task(second).at("wait").is_null(),"The alternate wait opened before the original answer");
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& text = input.event.at("text");
        if (text == originalText) {
            return route("clarify",{{"text","どちらの作業の名前ですか？"},{"candidates",{first,second}}});
        }
        check(text == secondName && input.event.at("reply").at("originalText") == originalText,
              "The stale-answer fixture received an unexpected instruction");
        return routing(Json::array({{{"kind","answer_wait"},{"taskId",second}}}),"resolved");
    };
    const auto answer = receive(writer,"original-answer",originalText);
    execution.runtime.routeConversation();
    const auto clarification = reply(store,answer);
    check(clarification.at("route") == "clarify" && clarification.at("candidates").size() == 2,
          "The original answer did not stop at its clarification");
    writer.operate(first,store.task(first).at("wait").at("id"),"answer",{{"text","別の製品名"}});
    execution.question(store,second,"report-name");
    const auto firstBefore = store.task(first), secondBefore = store.task(second);
    const auto launches = execution.t3.launches;
    const auto sentAnswers = execution.t3.answers.size();
    const auto selected = receive(writer,"stale-selection",secondName,clarification.at("messageId"));
    execution.runtime.routeConversation();
    const auto result = reply(store,selected).at("route");
    check((result == "clarify" || result == "respond") && store.task(first) == firstBefore &&
          store.task(second) == secondBefore && store.tasks("alpha").size() == 2 &&
          execution.t3.launches == launches && execution.t3.answers.size() == sentAnswers,
          "Resolving a stale clarification moved an expired answer to a different wait");
}

void boundedBotContextAndNamedHistory() {
    Fixture fixture;
    Store store(fixture.database()), writer(fixture.database());
    RuntimeFixture execution(store);
    constexpr auto oldName = "古い計測計画";
    constexpr auto namedQuestion = "「古い計測計画」のうち同じ負荷で比較した結果を要約して";
    constexpr auto foreignName = "Beta hidden project fixture";
    const auto old = task(writer,oldName);
    execution.finish(store,old,"Compare paired measurements using the same workload on both builds.");
    const auto original = store.latestTaskResult(old);
    for (int i = 0; i < 4; ++i) {
        const auto repeatedTitle = task(writer,oldName);
        execution.finish(store,repeatedTitle,"Single-build notes " + std::to_string(i));
    }
    for (int i = 0; i < 40; ++i) {
        const auto newest = task(writer,"New completed plan " + std::to_string(i));
        execution.finish(store,newest,"Latest plan " + std::to_string(i));
    }
    const auto foreign = task(writer,foreignName,"beta");
    execution.finish(store,foreign,"Beta-only result fixture");
    for (int i = 0; i < 35; ++i) task(writer,"Outstanding plan " + std::to_string(i));
    const auto before = store.tasks("alpha",0,200);
    const auto launchCount = execution.t3.launches;

    int routed = 0, answered = 0;
    execution.model.conversation = [&](const ModelInput& input) {
        const auto& event = input.event;
        if (input.tools.front() == "answer_conversation_result") {
            ++answered;
            check(event.at("text") == namedQuestion &&
                  event.at("source").at("messageId") == original.at("id") &&
                  event.at("source").at("text") == "Compare paired measurements using the same workload on both builds.",
                  "Named lookup did not send the older selected evidence and the user's question");
            return Decision{"answer_conversation_result",{{"text","同じ負荷条件で二つのビルドを計測し、比較する計画です。"}},{}};
        }
        ++routed;
        const auto encoded = event.dump() + input.history.dump();
        check(encoded.find(foreignName) == std::string::npos &&
              encoded.find("Beta-only result fixture") == std::string::npos,"Another Bot leaked into the routing context");
        const auto& tasks = event.at("tasks");
        const auto& results = event.at("results");
        check(tasks.size() < 35 && results.size() < 40 && event.at("moreTasks") == true &&
              encoded.size() < 128 * 1024,"Routing context grew with the Bot's full history");
        const auto source = std::ranges::find_if(results,[&](const Json& result) {
            return result.at("messageId") == original.at("id");
        });
        if (event.at("text") == "手元に現状メモはありますか") {
            check(source == results.end(),"The old result was included in an unqualified recent page");
            return route("respond",{{"text","対象を指定すると作業結果を確認できます。"}});
        }
        check(source != results.end() && std::ranges::count_if(results,[&](const Json& result) {
            return result.at("request") == oldName && result.at("messageId") != original.at("id");
        }) >= 4,"The explicitly named second page was absent from bounded context");
        return route("answer_result",{{"resultMessageId",source->at("messageId")}});
    };
    const auto probe = receive(writer,"without-name","手元に現状メモはありますか");
    execution.runtime.routeConversation();
    check(reply(store,probe).at("route") == "respond","Unqualified context probe failed");
    const auto named = receive(writer,"named",namedQuestion);
    execution.runtime.routeConversation();
    const auto response = reply(store,named);
    check(response.at("route") == "respond" && response.at("source").at("messageId") == original.at("id") &&
          response.at("text").get<std::string>().find("同じ負荷条件で二つのビルド") != std::string::npos &&
          routed == 2 && answered == 1 && store.tasks("alpha",0,200) == before && execution.t3.launches == launchCount,
          "Reading an older named result failed or changed work");
}

void legacyActiveWaitAnchor() {
    Fixture fixture;
    Store store(fixture.database());
    RuntimeFixture execution(store);
    const auto id = task(store,"Choose a release name");
    execution.question(store,id,"choose-name");
    const auto anchor = store.task(id).at("wait").at("messageId").get<Id>();
    check(anchor > 0,"The fixture did not record a wait journal entry");

    sqlite3* db = nullptr;
    check(sqlite3_open_v2(fixture.database().c_str(),&db,SQLITE_OPEN_READWRITE,nullptr) == SQLITE_OK,
          "Cannot prepare a legacy active wait");
    const auto rc = sqlite3_exec(db,"UPDATE tasks SET data=json_remove(data,'$.wait.messageId') WHERE state='waiting'",
                                 nullptr,nullptr,nullptr);
    const auto changed = sqlite3_changes(db);
    sqlite3_close(db);
    check(rc == SQLITE_OK && changed == 1,"The fixture did not remove exactly one stored wait anchor");

    const auto active = store.activeTasks("alpha");
    check(active.size() == 1 && active.front().at("id") == id &&
          active.front().at("wait").value("messageId",Id{}) == anchor,
          "The active task list lost the legacy question's journal anchor");
}

void legacyHistory() {
    Fixture fixture;
    sqlite3* db = nullptr;
    check(sqlite3_open(fixture.database().c_str(),&db) == SQLITE_OK,"Cannot create legacy DB");
    const auto rc = sqlite3_exec(db,R"SQL(
CREATE TABLE tasks(
  id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, environment_id TEXT NOT NULL,
  project_id TEXT NOT NULL, schedule_id TEXT, scheduled_at INTEGER,
  state TEXT NOT NULL, generation INTEGER NOT NULL, data TEXT NOT NULL,
  UNIQUE(bot_id,schedule_id,scheduled_at));
CREATE TABLE journal(id INTEGER PRIMARY KEY,task_id INTEGER NOT NULL REFERENCES tasks(id),
  time INTEGER NOT NULL,kind TEXT NOT NULL,data TEXT NOT NULL);
CREATE INDEX journal_task ON journal(task_id,id);
INSERT INTO tasks VALUES
  (1,'alpha','env','p',NULL,0,'completed',2,
     '{"botId":"alpha","environmentId":"env","projectId":"p","request":"Previous work","doneWhen":"Check all cases","wait":null}'),
  (2,'alpha','env','p',NULL,0,'queued',0,
     '{"botId":"alpha","environmentId":"env","projectId":"p","request":"Pending legacy work","doneWhen":"","wait":null}');
INSERT INTO journal VALUES(8,1,99,'report',json_object('report','1. First approach'||char(10)||'2. Chosen approach'));
INSERT INTO journal VALUES(9,1,100,'completed',json_object('text','Proposed two approaches'));
INSERT INTO journal VALUES(10,2,101,'request','{"text":"Pending legacy work","source":"manual"}');
)SQL",nullptr,nullptr,nullptr);
    sqlite3_close(db);
    check(rc == SQLITE_OK,"Legacy schema fixture failed");
    Store store(fixture.database());
    const auto completed = store.task(1);
    const auto& revisions = completed.at("revisions");
    check(completed.at("state") == "completed" && revisions.size() == 1 &&
          completed.at("currentRevision") == revisions.at(0).at("id") &&
          revisions.at(0).at("intent") == "Previous work" && revisions.at(0).at("doneWhen") == "Check all cases",
          "Legacy work lost its completion criteria or original request");
    const auto queued = store.task(2);
    check(queued.at("revisions").empty() && queued.at("state") == "queued",
          "Migration changed queued legacy work before its criteria were set");
    {
        sqlite3* oldReader = nullptr;
        check(sqlite3_open_v2(fixture.database().c_str(),&oldReader,SQLITE_OPEN_READONLY,nullptr) == SQLITE_OK,
              "Could not open migrated data with the legacy reader");
        unsigned rows = 0;
        const int oldQuery = sqlite3_exec(oldReader,
            "SELECT journal.id FROM journal JOIN tasks ON tasks.id=task_id WHERE bot_id='alpha' "
            "AND kind IN ('request','report','completed') ORDER BY journal.id DESC",
            [](void* count,int,char**,char**) { ++*static_cast<unsigned*>(count); return 0; },
            &rows,nullptr);
        sqlite3_close(oldReader);
        check(oldQuery == SQLITE_OK && rows == 3,"Migration broke the old unqualified Bot conversation query");
    }
    const auto latest = store.latestTaskResult(1);
    check(latest.at("id") == 9 && latest.at("data").at("text") == "Proposed two approaches",
          "The original completion disappeared from the migrated task");
    RuntimeFixture execution(store);
    execution.model.conversation = [](const ModelInput& input) {
        if (input.tools.front() == "route_conversation") {
            const auto& results = input.event.at("results");
            const auto source = std::ranges::find_if(results,[](const Json& result) {
                return result.at("messageId") == 8;
            });
            check(source != results.end(),"The migrated Session report was absent from routing evidence");
            return route("answer_result",{{"resultMessageId",source->at("messageId")},{"resultOption",2}});
        }
        check(input.event.at("source").at("text") == "2. Chosen approach","The model received the wrong legacy option");
        return Decision{"answer_conversation_result",{{"text","Chosen approach is the second recorded option. The old report gives no further details."}},{}};
    };
    const auto detailId = receive(store,"result","2案目を詳しく");
    execution.runtime.routeConversation();
    const auto detail = reply(store,detailId);
    const auto& explanation = detail.at("text").get_ref<const std::string&>();
    check(detail.at("route") == "respond" && detail.at("source").at("taskId") == 1 &&
          detail.at("source").at("messageId") == 8 && explanation.find("Chosen approach") != std::string::npos &&
          explanation.find("First approach") == std::string::npos && store.tasks("alpha").size() == 2,
          "The answer did not explain only the selected legacy option with its original source");
    Store refreshed(fixture.database());
    const auto reopened = refreshed.task(1);
    check(reopened.at("currentRevision") == completed.at("currentRevision") &&
          reopened.at("revisions").size() == 1 && refreshed.tasks("alpha").size() == 2,
          "Opening a migrated DB changed its current revision or duplicated legacy work");
}
}

int main() {
    try {
        retryAcrossConnections();
        clarificationAndStatus();
        compoundAndUnmatchedCancellation();
        independentWorkAndSupplement();
        naturalWaitAnswer();
        clarificationKeepsTheUserInstruction();
        implicitClarificationRequiresAnEligibleReply();
        longResultOptionsStayAvailable();
        explicitFactsReplaceResultClarificationAtomically();
        followUpsKeepThePriorResult();
        comparisonsDoNotSelectOneOfSeveralOptions();
        excerptRepliesExpandToWholeResult();
        resultTargetMustComeFromTheCandidatesOrReply();
        clarificationCannotTransferAnExpiredAnswer();
        boundedBotContextAndNamedHistory();
        legacyHistory();
        legacyActiveWaitAnchor();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
