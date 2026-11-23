#include "flock/policy.hpp"
#include "flock/workspace_tools.hpp"
#include "model.hpp"
#include "../src/conversation_storage.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <unistd.h>

using namespace flockv0;

namespace {
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

constexpr std::string_view portal = "https://audit.example.test/";
constexpr auto question = "Who signs the audit?";
constexpr auto archivePrefix = "LEDGER VERIFIED";
constexpr auto archiveCode = "NZ-7184-Q";
constexpr auto firstTicket = "NORTH-413";
constexpr auto finalTicket = "SOUTH-982";
constexpr int pageCount = 6;
const Person owner{"fixture-owner",true};

void check(bool condition, std::string_view description) {
    if (!condition) throw std::runtime_error(std::string(description));
}

void verifyToolRetention() {
    using conversation_storage::projectToolHistory;
    const auto record = [](Id step, std::string_view tool, Json args, Json result) {
        return Json{{"_ref",{{"messageId",42},{"step",step},{"callIndex",1}}},
                    {"tool",tool},{"args",std::move(args)},{"result",std::move(result)}};
    };
    const std::string escapedChunk(6000,'\x01');
    const Json ref{{"messageId",42},{"step",1},{"callIndex",1}};
    Json calls = Json::array({
        record(1,"read_result",Json::object(),{{"ok",true},{"text",escapedChunk}}),
        record(2,"read_tool_result",ref,{{"ok",true}}),
        record(3,"find_tasks",Json::object(),{{"ok",true}})
    });
    projectToolHistory(calls,nullptr);
    check(calls.at(1).at("result").at("result").at("text") == escapedChunk,
          "Reopened evidence over the earlier-results budget disappeared after a lookup");

    const std::string payload(50000,'x');
    calls = Json::array({
        record(1,"find_tasks",Json::object(),{{"ok",true},{"before",11},{"tasks",payload}}),
        record(2,"list_schedules",Json::object(),{{"ok",true},{"before",22},{"automations",payload}}),
        record(3,"memory_history",Json::object(),{{"ok",true},{"before",33},{"proposalsBefore",44},{"revisions",payload}}),
        record(4,"find_tasks",Json::object(),{{"ok",true}}),
        record(5,"find_tasks",Json::object(),{{"ok",true}})
    });
    projectToolHistory(calls,nullptr);
    for (std::size_t i = 0; i < 3; ++i)
        check(calls.at(i).at("result").at("continuation").at("before") == 11 * (i+1),
              "A compact first-party page lost its continuation");
    check(calls.at(2).at("result").at("continuation").at("proposalsBefore") == 44,
          "A compact memory page lost its proposal cursor");

    calls = Json::array();
    const Json longArgs{{"cursor",std::string(3800,'c')}};
    for (Id step = 1; step <= 224; ++step)
        calls.push_back(record(step,"expand_context",longArgs,{{"ok",false}}));
    calls.push_back(record(225,"expand_context",longArgs,{{"ok",false},{"notRun",true}}));
    projectToolHistory(calls,nullptr);
    check(Json(calls.dump()).dump().size() <= 768 * 1024 + 2 && calls.back().at("step") == 225 &&
          calls.back().at("args").empty() && calls.back().at("argumentsOmitted") == true,
          "Compact rows exceeded the model budget or kept rejected arguments");
}

double elapsedMs(Clock::time_point begin) {
    return std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
}

std::string field(const Json& object, const char* key) {
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : "";
}

std::string lineAfter(std::string_view text, std::string_view label) {
    const auto start = text.find(label);
    if (start == text.npos) return {};
    const auto rest = text.substr(start+label.size());
    return std::string(rest.substr(0,rest.find('\n')));
}

Decision action(std::string tool, Json args = Json::object()) {
    return {std::move(tool),std::move(args),"Use fixture evidence to answer the request"};
}

Decision reply(std::string text) {
    return action("reply",{{"text",std::move(text)}});
}

Decision fetch(const std::string& url) {
    return action("web_fetch",{{"url",url}});
}

std::string pageUrl(int page) {
    return std::string(portal) + "page-" + std::to_string(page);
}

std::string auditRows(int page, int length) {
    std::string text;
    for (int row = 0; static_cast<int>(text.size()) < length; ++row) {
        text += "Northstar audit row " + std::to_string(page*1000+row) +
                ": receiving manifest matched the inventory and retained the original evidence.\n";
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

struct FakeWeb : WebTools {
    std::mutex mutex;
    std::map<std::string,int> fetches;

    Json search(std::string_view) override { throw Conflict("Fixture permits only its audit pages"); }
    Json fetch(std::string_view url) override {
        const std::lock_guard lock(mutex);
        ++fetches[std::string(url)];
        if (url == std::string(portal) + "status") return {{"status","PASS"}};
        for (int page = 1; page <= pageCount; ++page) {
            if (url != pageUrl(page)) continue;
            const auto ticket = page == 1 ? firstTicket : page == pageCount ? finalTicket : "INTERMEDIATE";
            return {{"ticket",fetches.at(std::string(url)) == 1 ? ticket : "REGENERATED"},
                    {"nextUrl",page == pageCount ? "" : pageUrl(page+1)},{"rows",auditRows(page,8500)}};
        }
        throw Conflict("Fixture permits only its audit pages");
    }
};

struct NoT3 : T3 {
    std::string authGeneration(const std::string&,const Json&) override { return "fixture"; }
    Delivery send(const Command&) override { throw Conflict("Fixture cannot launch a Session"); }
    Delivery reconcile(const Command&) override { return {}; }
    Observation observe(const std::string&,const Json&,const RunRef&) override { return {}; }
    InterruptOutcome interrupt(const std::string&,const Json&,const RunRef&) override {
        return InterruptOutcome::NoActiveRun;
    }
};

std::size_t wireBytes(const Json& value) {
    return Json(value.dump()).dump().size()-2;
}

struct FixtureModel : Model {
    using Script = std::function<Decision(const Json&)>;
    Script script;
    Model* live = nullptr;
    bool measuring = false;
    Seconds now = 1795449600;
    std::string previousContext;
    Json calls = Json::array();

    ModelTurn decide(const ModelInput& input) override {
        if (!measuring)
            return {{script({})}};
        ++now;
        const auto begin = Clock::now();
        const auto request = app::responseRequest(input);
        const auto& messages = request.at("input");
        check(messages.size() == 1 && messages.front().at("role") == "user","Expected one user context");
        const auto& contextText = messages.front().at("content").front().at("text").get_ref<const std::string&>();
        const auto context = Json::parse(contextText);
        const auto prefixBytes = std::ranges::mismatch(previousContext,contextText).in1-previousContext.begin();
        previousContext = contextText;
        auto& record = calls.emplace_back(Json{
            {"localRequestBytes",request.dump().size()},{"historyBytes",wireBytes(context.at("history"))},
            {"toolHistoryBytes",wireBytes(context.at("event").at("turn"))},
            {"commonContextPrefixBytes",prefixBytes},{"providerInputTokens",nullptr},
            {"providerCachedInputTokens",nullptr},{"providerCacheWriteTokens",nullptr},{"actions",Json::array()}});
        const auto finish = [&](const Json& usage) {
            record["localCallMs"] = elapsedMs(begin);
            if (usage.contains("input_tokens")) record["providerInputTokens"] = usage.at("input_tokens");
            if (const auto details = usage.find("input_tokens_details"); details != usage.end()) {
                if (details->contains("cached_tokens")) record["providerCachedInputTokens"] = details->at("cached_tokens");
                if (details->contains("cache_write_tokens")) record["providerCacheWriteTokens"] = details->at("cache_write_tokens");
            }
            if (usage.contains("wallMs")) record["adapterWallMs"] = usage.at("wallMs");
        };
        try {
            ModelTurn turn;
            if (live) {
                turn = live->decide(input);
            } else {
                const auto decision = script(context);
                auto args = decision.args;
                args["reason"] = decision.reason;
                turn = app::responseDecision(Json::array({{{"type","function_call"},{"namespace","flock"},
                    {"status","completed"},{"name",decision.tool},{"arguments",args.dump()}}}),request);
            }
            for (const auto& call : turn.calls) {
                record["actions"].push_back(call.tool);
                if (live && call.tool != "reply" && call.tool != "web_fetch" && call.tool != "find_tasks" &&
                    call.tool != "expand_context" && call.tool != "read_tool_result")
                    throw ModelError("Live fixture permits only its read tools and reply",false,turn.usage);
            }
            finish(turn.usage);
            return turn;
        } catch (const ModelError& error) {
            finish(error.usage);
            record["error"] = error.what();
            throw;
        } catch (const std::exception& error) {
            finish(Json::object());
            record["error"] = error.what();
            throw;
        }
    }
};

struct Directory {
    fs::path path;
    Directory() {
        auto pattern = (fs::temp_directory_path()/"flock-model-context-XXXXXX").string();
        const auto* created = mkdtemp(pattern.data());
        check(created != nullptr,"Could not create fixture directory");
        path = created;
    }
    ~Directory() {
        std::error_code error;
        fs::remove_all(path,error);
    }
};

struct Fixture {
    Directory directory;
    Store store{directory.path/"state.db"};
    FixtureModel model;
    NoT3 t3;
    std::shared_ptr<FakeWeb> web = std::make_shared<FakeWeb>();
    Runtime runtime{store,model,t3,[](auto) { return std::string{}; },
                    {.clock = [this] { return model.now; },.botTools = std::make_shared<BotTools>(BotTools{.skills = {},.web = web,.mcp = {}})}};
    Json config;
    int sequence = 0;

    explicit Fixture(std::string_view slug, Model* live)
        : config{{"modelProvider","chatgpt"},{"model",slug},{"modelCallLimit",20}} {
        model.live = live;
    }

    Intake intake(std::string request = {}) const {
        return {"audit",std::move(request),"fixture-environment","fixture-project",config};
    }

    Json send(std::string_view message) {
        const auto id = store.receive(owner,intake(),std::to_string(++sequence),message).at("messageId");
        runtime.runConversations();
        for (const auto& item : store.conversation("audit",0,100)) {
            const auto& data = item.at("data");
            if (item.at("kind") == "conversation_reply" && data.at("replyToMessageId") == id)
                return {{"reply",data},{"id",item.at("id")}};
        }
        return Json::object();
    }

    Id seed(std::string_view message, std::string answer) {
        model.script = [answer = std::move(answer)](const auto&) { return reply(answer); };
        return send(message).at("id").get<Id>();
    }
};

std::string historyValue(const Json& history, std::string_view label) {
    for (auto entry = history.rbegin(); entry != history.rend(); ++entry) {
        if (entry->value("role","") != "user") continue;
        if (auto value = lineAfter(field(*entry,"text"),label); !value.empty()) return value;
    }
    return {};
}

struct Case {
    std::string_view name, message, expected;
    FixtureModel::Script script;
    std::function<bool()> evidenceChecked = [] { return true; };
};

Case shortRequest() {
    return {"short","Reply with only the word:\nMARIGOLD","MARIGOLD",[](const Json& context) {
        return reply(lineAfter(field(context.at("event"),"text"),"word:\n"));
    }};
}

Case longFollowup(Fixture& f) {
    f.model.script = [](const auto&) { return action("ask_user",{{"question",question}}); };
    const auto task = f.store.submit(f.intake("Northstar audit"));
    for (int tick = 0; tick < 5 && !f.store.task(task).at("wait").is_object(); ++tick) f.runtime.tick();
    check(field(f.store.task(task).at("wait"),"text") == question,"Fixture did not open the audit question");
    f.seed("Kickoff code: ORCHID-45\nAudit scope: production only","I recorded the kickoff and original scope.");
    for (int i = 0; i < 9; ++i)
        f.seed("Prior Northstar audit material.\n"+auditRows(i,600),auditRows(i,1450));
    f.seed("Correction to the Northstar audit.\nAudit scope: staging only","I recorded the scope correction.");
    return {"long-followup",
        "For the Northstar audit, read https://audit.example.test/status to check the status. Use the latest audit scope "
        "and the kickoff code from our earlier conversation. Tell me the unresolved task question without answering it. "
        "Reply with exactly the scope, kickoff code, status, and question separated by ' | '.",
        "staging only | ORCHID-45 | PASS | Who signs the audit?",
        [](const Json& context) {
            const auto& turn = context.at("event").at("turn");
            if (turn.empty()) return fetch(std::string(portal) + "status");
            const auto& history = context.at("history");
            const auto& tasks = context.at("task").at("tasks");
            const auto& wait = tasks.at(0).at("wait");
            const auto status = field(turn.back().at("result").at("data"),"status");
            return reply(historyValue(history,"Audit scope: ")+" | "+historyValue(history,"Kickoff code: ") +
                         " | "+status+" | "+field(wait,"text"));
        },
        [&f,task] {
            const auto status = f.web->fetches.find(std::string(portal) + "status");
            return status != f.web->fetches.end() && status->second == 1 &&
                field(f.store.task(task).at("wait"),"text") == question;
        }};
}

Case largeToolResults(Fixture& f) {
    return {"large-results",
        "Read the synthetic audit manifest at https://audit.example.test/page-1. Each manifest has a ticket and a nextUrl. "
        "Read each nextUrl in sequence until it is empty. Ignore the rows. Recover any missing ticket from Flock's "
        "saved tool evidence. Reply with only the first and last "
        "tickets separated by ' / '.",
        "NORTH-413 / SOUTH-982",
        [](const Json& context) {
            const auto& turn = context.at("event").at("turn");
            if (turn.empty()) return fetch(pageUrl(1));
            const auto& latest = turn.back();
            const auto& first = turn.front().at("result");
            if (latest.at("tool") == "read_tool_result") {
                const auto& recovered = latest.at("result");
                const auto& data = recovered.at("result").at("data");
                check(recovered.at("args").at("url") == pageUrl(1) && recovered.at("resultRef") == first.at("resultRef") &&
                      data == Json{{"rows",auditRows(1,8500)},{"ticket",firstTicket},{"nextUrl",pageUrl(2)}},
                      "Reread did not recover the original result and reference");
                const auto& terminal = turn.at(turn.size()-2).at("result").at("data");
                return reply(field(data,"ticket")+" / "+field(terminal,"ticket"));
            }
            const auto& data = latest.at("result").at("data");
            if (const auto next = field(data,"nextUrl"); !next.empty()) return fetch(next);
            if (first.value("evidenceOmitted",false)) {
                check(!first.contains("data") && !first.contains("continuation"),
                      "Omission exposed arbitrary external fields as result data or pagination");
                return action("read_tool_result",first.at("resultRef"));
            }
            return reply(field(first.at("data"),"ticket")+" / "+field(data,"ticket"));
        },
        [&f] {
            return f.web->fetches.size() == pageCount &&
                std::ranges::all_of(f.web->fetches,[](const auto& entry) { return entry.second == 1; });
        }};
}

Case oldReference(Fixture& f) {
    f.seed("For any future archive check, use this exact reply prefix: "+std::string(archivePrefix),"Understood.");
    const auto evidence = f.seed("Record the Northstar handoff for future audit checks.",
                                 "The Northstar handoff batch code was recorded as:\n"+std::string(archiveCode));
    f.model.script = [evidence,step = 0](const auto&) mutable {
        if (step++ == 0) return action("compact_conversation",{{"endMessageId",evidence},
            {"summary","Archive reply prefix: "+std::string(archivePrefix)+
                       "\nThe old Northstar handoff has a saved batch code. Expand the original Bot message at message ID "
                       +std::to_string(evidence)+" before reporting it."}});
        return reply("Archived.");
    };
    f.send("Compact the old archive notes.");
    check(f.store.compactions("audit").size() == 1,"Fixture did not save the cumulative summary");
    for (int i = 0; i < 9; ++i)
        f.seed("Earlier unrelated audit discussion.\n"+auditRows(i,600),auditRows(i,1450));
    return {"old-reference",
        "What batch code was recorded in the Northstar handoff? Reply in my established archive format, with the prefix "
        "followed by a colon and the exact code. Use the original saved evidence.",
        "LEDGER VERIFIED: NZ-7184-Q",
        [](const Json& context) {
            const auto& history = context.at("history");
            const auto summary = std::ranges::find_if(history,[](const auto& item) {
                return item.value("kind","") == "conversation_compaction";
            });
            check(summary != history.end(),"Cumulative summary omitted");
            const auto prefix = lineAfter(field(*summary,"summary"),"Archive reply prefix: ");
            const auto& turn = context.at("event").at("turn");
            check(history.dump().find(archiveCode) == std::string::npos,"Archived code was unexpectedly sent inline");
            if (turn.empty()) return action("expand_context",{{"compactionId",summary->at("compactionId")},
                {"startMessageId",summary->at("endMessageId")},{"endMessageId",summary->at("endMessageId")}});
            const auto& result = turn.back().at("result");
            const auto code = lineAfter(field(result.at("messages").at(0),"text"),"recorded as:\n");
            return reply(prefix+": "+code);
        },
        [&f] { return std::ranges::any_of(f.model.calls,[](const auto& record) {
            return std::ranges::find(record.at("actions"),"expand_context") != record.at("actions").end();
        }); }};
}

Json totalUsage(const Json& calls, const char* key) {
    if (calls.empty()) return nullptr;
    Id total = 0;
    for (const auto& call : calls) {
        const auto& count = call.at(key);
        if (!count.is_number_integer()) return nullptr;
        total += count.get<Id>();
    }
    return total;
}

bool measure(Fixture& fixture, const Case& test, std::string_view slug, bool live) {
    fixture.model.script = test.script;
    fixture.model.measuring = true;
    const auto begin = Clock::now();
    const auto message = fixture.send(test.message);
    const auto wallMs = elapsedMs(begin);
    const auto reply = message.value("reply",Json::object());
    const auto actual = field(reply,"text");
    const auto success = field(reply,"route") == "reply" && actual == test.expected && test.evidenceChecked();
    std::size_t totalBytes = 0;
    for (const auto& call : fixture.model.calls)
        totalBytes += call.at("localRequestBytes").get<std::size_t>();
    std::cout << Json{{"case",test.name},{"mode",live ? "live" : "fixed"},{"model",slug},
        {"calls",fixture.model.calls},{"callCount",fixture.model.calls.size()},{"totalLocalRequestBytes",totalBytes},
        {"totalProviderInputTokens",totalUsage(fixture.model.calls,"providerInputTokens")},
        {"totalProviderCachedInputTokens",totalUsage(fixture.model.calls,"providerCachedInputTokens")},
        {"totalProviderCacheWriteTokens",totalUsage(fixture.model.calls,"providerCacheWriteTokens")},{"wallMs",wallMs},
        {"success",success},{"reply",actual},{"expected",test.expected}}.dump() << '\n';
    return success;
}
}

int main(int argc, char** argv) {
    try {
        const bool baseline = argc == 2 && std::string_view(argv[1]) == "--measure-baseline";
        const bool live = argc == 5 && std::string_view(argv[1]) == "--live";
        check(argc == 1 || baseline || live,
              "Usage: flock_model_context [--measure-baseline | --live PROFILE_DIR MODEL CASE]");
        const std::string_view slug = live ? argv[3] : "gpt-6.2-sol";
        const std::string_view selected = live ? argv[4] : "all";
        std::optional<app::ChatGPTModel> provider;
        if (live) provider.emplace(argv[2]);
        auto* adapter = live ? &*provider : nullptr;
        bool success = true, matched = false;
        const auto run = [&](std::string_view name, const auto& build) {
            if (selected != "all" && selected != name) return;
            matched = true;
            Fixture fixture(slug,adapter);
            success = measure(fixture,build(fixture),slug,live) && success;
        };
        if (!baseline && !live) verifyToolRetention();
        run("short",[](auto&) { return shortRequest(); });
        run("long-followup",longFollowup);
        run("large-results",largeToolResults);
        run("old-reference",oldReference);
        check(matched,"Unknown fixture case");
        return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
