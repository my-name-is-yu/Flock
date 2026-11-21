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
        if (text == "今どこまでできた？") return route("status");
        if (text == "計測作業の方です")
            return routing(Json::array({{{"kind","cancel_task"},{"taskId",b}}}),"resolved");
        check(text == "計測作業を再開して","Unexpected clarification fixture input");
        const auto& results = input.event.at("results");
        const auto source = std::ranges::find_if(results,[&](const Json& result) {
            return result.at("taskId") == b && result.at("state") == "cancelled";
        });
        check(source != results.end(),"The cancelled source was missing");
        return route("create_task",{{"resultMessageId",source->at("messageId")}});
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
            return route("answer_result",{{"resultMessageId",source->at("messageId")}});
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
        clarificationCannotTransferAnExpiredAnswer();
        boundedBotContextAndNamedHistory();
        legacyHistory();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
