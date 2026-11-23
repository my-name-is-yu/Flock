#include "flock/policy.hpp"
#include "flock/workspace_tools.hpp"
#include "model.hpp"

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
constexpr int pageCount = 22;
constexpr int pageEvidenceBytes = 9600;
constexpr int limitPages = 22;
constexpr int seriesPages = 7;
constexpr int seriesCount = 5;
constexpr auto openingPrefix = "ORCHARD VERIFIED";
constexpr auto revisedPrefix = "CYPRESS VERIFIED";
constexpr auto reviewSeal = "CIRRUS-2187-Q";
const Person owner{"fixture-owner",true};

void check(bool condition, std::string_view description) {
    if (!condition) throw std::runtime_error(std::string(description));
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

int chainLength(int series) {
    return series == 0 ? limitPages : seriesPages;
}

std::string chainUrl(int series, int page) {
    const auto root = std::string(portal) + "series-" + std::to_string(series) + "/";
    if (page == 1) return root + "start";
    return root + "manifest-" + std::to_string((137 * page + 71 * series) % 1009);
}

std::string chainTicket(int series, int page) {
    return "GATE-" + std::to_string(8456 + 491 * series + 157 * page);
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
                    {"nextUrl",page == pageCount ? "" : pageUrl(page+1)},{"rows",auditRows(page,pageEvidenceBytes)}};
        }
        for (int series = 0; series <= seriesCount; ++series) {
            for (int page = 1; page <= chainLength(series); ++page) {
                if (url != chainUrl(series,page)) continue;
                const bool last = page == chainLength(series);
                auto rows = auditRows(series*100+page,8500);
                if (series == seriesCount && page == 3)
                    rows += "\nReview seal: " + std::string(reviewSeal);
                Json data{{"ticket",fetches.at(std::string(url)) == 1 ? chainTicket(series,page) : "REGENERATED"},
                          {"nextUrl",last ? "" : chainUrl(series,page+1)},{"rows",std::move(rows)}};
                if (series == seriesCount && last) data["sealSource"] = chainUrl(series,3);
                return data;
            }
        }
        throw Conflict("Fixture permits only its audit pages");
    }
    bool fetchedChainOnce(int series) const {
        for (int page = 1; page <= chainLength(series); ++page) {
            const auto entry = fetches.find(chainUrl(series,page));
            if (entry == fetches.end() || entry->second != 1) return false;
        }
        return true;
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

std::size_t commonBytes(std::string_view previous, std::string_view current) {
    return std::ranges::mismatch(previous,current).in1-previous.begin();
}

struct PromptPart {
    std::string section, field, body;
    bool operator==(const PromptPart&) const = default;
};

void addContextParts(std::vector<PromptPart>& parts, const nlohmann::ordered_json& block) {
    for (const auto& [key,value] : block.items()) {
        if (key == "event" && value.is_object()) {
            for (const auto& [eventKey,eventValue] : value.items()) {
                const auto section = eventKey == "turn" ? "tool_results" :
                    eventKey == "text" || eventKey == "messageId" || eventKey == "replyTo" ? "user_message" :
                    "current_state";
                parts.push_back({section,"event."+eventKey,eventValue.dump()});
            }
            continue;
        }
        const auto section = key == "soul" || key == "memory" ? "instructions" :
            key == "history" ? "history" : "current_state";
        parts.push_back({section,key,value.dump()});
    }
}

struct PromptSnapshot {
    std::string request, input, contextText;
    std::vector<PromptPart> parts;
    Json context = Json::object();
    Json messages;

    explicit PromptSnapshot(const Json& payload)
        : request(payload.dump()),input(payload.at("input").dump()),messages(payload.at("input")) {
        const auto instructions = field(payload,"instructions");
        const bool clock = instructions.starts_with("Current event.currentTime");
        if (!instructions.empty() && !clock)
            parts.push_back({"instructions","instructions",payload.at("instructions").dump()});
        for (const auto& message : messages) {
            if (field(message,"role") == "developer")
                parts.push_back({"instructions","input.developer",message.at("content").dump()});
        }
        parts.push_back({"tool_definitions","tools",payload.at("tools").dump()});
        std::map<std::string,Json> calls;
        for (const auto& message : messages) {
            const auto type = field(message,"type");
            if (type == "function_call") {
                calls.emplace(field(message,"call_id"),Json{{"tool",message.at("name")},
                    {"args",Json::parse(field(message,"arguments"))}});
                contextText += message.dump() + '\n';
                parts.push_back({"tool_results","function_call",message.dump()});
                continue;
            }
            if (type == "function_call_output") {
                auto entry = std::move(calls.at(field(message,"call_id")));
                entry.update(Json::parse(field(message,"output")));
                context.at("event").at("turn").push_back(std::move(entry));
                contextText += message.dump() + '\n';
                parts.push_back({"tool_results","function_call_output",message.dump()});
                continue;
            }
            const auto role = field(message,"role");
            if (role != "user" && role != "assistant") continue;
            for (const auto& block : message.at("content")) {
                const auto text = field(block,"text");
                if (!text.starts_with('{')) {
                    parts.push_back({"current_state","input.content",block.dump()});
                    continue;
                }
                contextText += text + '\n';
                auto fields = nlohmann::ordered_json::parse(text);
                if (fields.contains("historyEntry")) {
                    auto& history = context.at("history");
                    parts.push_back({"history","history["+std::to_string(history.size())+"]",fields.at("historyEntry").dump()});
                    history.push_back(Json(std::move(fields.at("historyEntry"))));
                    continue;
                }
                if (fields.contains("event") && fields.at("event").contains("turn") &&
                    context.contains("event") && context.at("event").contains("turn")) {
                    auto& turn = context.at("event").at("turn");
                    for (auto& result : fields.at("event").at("turn")) {
                        parts.push_back({"tool_results","event.turn["+std::to_string(turn.size())+"]",result.dump()});
                        turn.push_back(Json(std::move(result)));
                    }
                    fields.at("event").erase("turn");
                }
                context.update(Json(fields),true);
                addContextParts(parts,fields);
            }
        }
        if (clock) parts.push_back({"current_state","event.currentTime",instructions});
    }
};

void compareEntries(Json& record, std::string_view name, const Json& previous, const Json& current) {
    const auto [firstOld,firstNew] = std::ranges::mismatch(previous,current);
    const auto common = firstOld - previous.begin();
    const auto key = std::string(name);
    record[key+"CommonEntries"] = common;
    record[key+"Change"] = firstOld != previous.end() ?
        (firstNew != current.end() ? "rewritten" : "truncated") :
        (firstNew != current.end() ? "appended" : "unchanged");
}

struct FixtureModel : Model {
    using Script = std::function<Decision(const Json&)>;
    Script script;
    Model* live = nullptr;
    bool measuring = false;
    bool allowCompaction = false;
    Seconds now = 1795449600;
    int turnIndex = 0;
    int turnCallIndex = 0;
    std::optional<PromptSnapshot> previous;
    Json calls = Json::array();

    ModelTurn decide(const ModelInput& input) override {
        if (!measuring)
            return {{script({})}};
        ++now;
        const auto begin = Clock::now();
        const auto request = app::responseRequest(input);
        PromptSnapshot prompt(request);
        const auto& history = prompt.context.at("history");
        const auto& toolHistory = prompt.context.at("event").at("turn");
        const auto summary = std::ranges::find_if(history,[](const auto& entry) {
            return entry.value("kind","") == "conversation_compaction";
        });
        auto& record = calls.emplace_back(Json{
            {"callIndex",calls.size()+1},{"turnIndex",turnIndex},{"turnCallIndex",++turnCallIndex},
            {"localRequestBytes",prompt.request.size()},{"transportRequestBytes",nullptr},
            {"historyBytes",wireBytes(history)},{"historyEntries",history.size()},
            {"toolHistoryBytes",wireBytes(toolHistory)},{"toolResults",toolHistory.size()},
            {"compactionId",summary == history.end() ? Json(nullptr) : summary->at("compactionId")},
            {"omittedHistoryEntries",std::ranges::count_if(history,[](const auto& entry) {
                return entry.value("evidenceOmitted",false);
            })},
            {"omittedToolResults",std::ranges::count_if(toolHistory,[](const auto& entry) {
                return entry.at("result").value("evidenceOmitted",false);
            })},
            {"providerInputTokens",nullptr},{"providerCachedInputTokens",nullptr},{"providerUncachedInputTokens",nullptr},
            {"providerCacheWriteTokens",nullptr},{"actions",Json::array()}});
        record["firstInTurn"] = turnCallIndex == 1;
        record["commonRequestPrefixBytes"] = previous ? commonBytes(previous->request,prompt.request) : 0;
        record["commonInputPrefixBytes"] = previous ? commonBytes(previous->input,prompt.input) : 0;
        record["commonContextPrefixBytes"] = previous ? commonBytes(previous->contextText,prompt.contextText) : 0;
        record["firstChangedSection"] = nullptr;
        record["firstChangedField"] = nullptr;
        if (previous) {
            const auto [previousMessage,currentMessage] = std::ranges::mismatch(previous->messages,prompt.messages);
            record["commonInputMessages"] = previousMessage - previous->messages.begin();
            record["previousInputMessagesArePrefix"] = previousMessage == previous->messages.end();
            const auto [firstOld,firstNew] = std::ranges::mismatch(previous->parts,prompt.parts);
            if (firstNew != prompt.parts.end() || firstOld != previous->parts.end()) {
                const auto& changed = firstNew != prompt.parts.end() ? *firstNew : *firstOld;
                record["firstChangedSection"] = changed.section;
                record["firstChangedField"] = changed.field;
            }
            compareEntries(record,"history",previous->context.at("history"),history);
            compareEntries(record,"toolResults",previous->context.at("event").at("turn"),toolHistory);
        }
        previous = std::move(prompt);
        const auto finish = [&](const Json& usage) {
            record["localCallMs"] = elapsedMs(begin);
            if (usage.contains("input_tokens")) record["providerInputTokens"] = usage.at("input_tokens");
            if (const auto details = usage.find("input_tokens_details"); details != usage.end()) {
                if (details->contains("cached_tokens")) record["providerCachedInputTokens"] = details->at("cached_tokens");
                if (details->contains("cache_write_tokens")) record["providerCacheWriteTokens"] = details->at("cache_write_tokens");
            }
            if (record.at("providerInputTokens").is_number_integer() &&
                record.at("providerCachedInputTokens").is_number_integer())
                record["providerUncachedInputTokens"] = record.at("providerInputTokens").get<Id>() -
                    record.at("providerCachedInputTokens").get<Id>();
            if (usage.contains("transportRequestBytes")) record["transportRequestBytes"] = usage.at("transportRequestBytes");
            if (usage.contains("wallMs")) record["adapterWallMs"] = usage.at("wallMs");
        };
        try {
            ModelTurn turn;
            if (live) {
                turn = live->decide(input);
            } else {
                const auto decision = script(previous->context);
                auto args = decision.args;
                args["reason"] = decision.reason;
                turn = app::responseDecision(Json::array({{{"type","function_call"},{"namespace","flock"},
                    {"status","completed"},{"name",decision.tool},{"arguments",args.dump()}}}),request);
            }
            for (const auto& call : turn.calls) {
                record["actions"].push_back(call.tool);
                if (live && call.tool != "reply" && call.tool != "web_fetch" && call.tool != "find_tasks" &&
                    call.tool != "expand_context" && call.tool != "read_tool_result" &&
                    !(allowCompaction && call.tool == "compact_conversation"))
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
        if (model.measuring) {
            ++model.turnIndex;
            model.turnCallIndex = 0;
        }
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
    std::string name, message, expected;
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

Case largeToolResults(Fixture& f, bool requireReread) {
    f.config["modelCallLimit"] = 48;
    return {"large-results",
        "Read the synthetic audit manifest at https://audit.example.test/page-1. Each manifest has a ticket and a nextUrl. "
        "Read each nextUrl in sequence until it is empty. Ignore the rows. If earlier saved results are omitted, recover "
        "them from Flock's saved tool evidence instead of fetching the page again. Reply with only the first and last "
        "tickets separated by ' / '.",
        "NORTH-413 / SOUTH-982",
        [](const Json& context) {
            const auto& turn = context.at("event").at("turn");
            if (turn.empty()) return fetch(pageUrl(1));
            const auto& latest = turn.back();
            const auto& first = turn.front().at("result");
            if (latest.at("tool") == "read_tool_result") {
                const auto& recovered = latest.at("result");
                const auto url = field(recovered.at("args"),"url");
                int page = 1;
                while (page <= pageCount && pageUrl(page) != url) ++page;
                check(page <= pageCount &&
                      field(recovered.at("result").at("data"),"rows") == auditRows(page,pageEvidenceBytes),
                      "Reread did not recover the original full result");
                const auto& terminal = turn.at(turn.size()-2).at("result").at("data");
                return reply(field(first.at("data"),"ticket")+" / "+field(terminal,"ticket"));
            }
            const auto& data = latest.at("result").at("data");
            if (const auto next = field(data,"nextUrl"); !next.empty()) return fetch(next);
            for (const auto& step : turn) {
                const auto& result = step.at("result");
                if (result.value("evidenceOmitted",false)) {
                    check(result.at("data").contains("ticket") && !result.at("data").contains("rows"),
                          "Omitted result lost its ticket or still contains the large body");
                    return action("read_tool_result",result.at("resultRef"));
                }
            }
            return reply(field(first.at("data"),"ticket")+" / "+field(data,"ticket"));
        },
        [&f,requireReread] {
            if (f.web->fetches.size() != pageCount ||
                !std::ranges::all_of(f.web->fetches,[](const auto& entry) { return entry.second == 1; })) return false;
            return !requireReread || std::ranges::any_of(f.model.calls,[](const auto& record) {
                return std::ranges::find(record.at("actions"),"read_tool_result") != record.at("actions").end();
            });
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

Case qualityReplay(Fixture& f) {
    static constexpr std::string_view receipt = "17 crates sealed at bay 04 on tag Q7-MER-081.";
    static const auto statusUrl = std::string(portal) + "quality-status";
    const auto evidence = f.seed("Archive the receiving receipt.\nAudit scope: production only",
                                 "Receipt: "+std::string(receipt));
    f.model.script = [evidence,compacted = false](const auto&) mutable {
        if (compacted) return reply("Archived.");
        compacted = true;
        return action("compact_conversation",{{"endMessageId",evidence},
            {"summary","The previous audit scope was production only. The original receiving receipt is in the Bot reply at endMessageId."}});
    };
    f.send("Compact the old receipt conversation.");
    check(f.store.compactions("audit").size() == 1,"Fixture did not compact the original receipt");
    f.seed("Correction for the audit.\nAudit scope: staging only","I recorded the scope correction.");

    return {"quality-replay",
        "An old dashboard label said PASS. Check the current status once at "+statusUrl+
        ". If that fetch fails, report UNVERIFIED. Use the latest audit scope from our conversation. "
        "Use expand_context to read the original Bot receipt at the compaction's endMessageId "
        "with that exact ID as both bounds. Return only the current scope, the exact sentence following "
        "'Receipt: ' in the original reply, and the verified current status, separated by ' | '.",
        "staging only | "+std::string(receipt)+" | UNVERIFIED",
        [](const Json& context) {
            const auto& history = context.at("history");
            const auto summary = std::ranges::find_if(history,[](const auto& item) {
                return item.value("kind","") == "conversation_compaction";
            });
            check(summary != history.end() && field(*summary,"summary").find("production only") != std::string::npos &&
                  history.dump().find(receipt) == std::string::npos,"The fixture lost the archived source boundary");
            const auto& turn = context.at("event").at("turn");
            const auto expanded = std::ranges::find_if(turn,[](const auto& item) {
                return item.at("tool") == "expand_context";
            });
            if (expanded == turn.end())
                return action("expand_context",{{"compactionId",summary->at("compactionId")},
                    {"startMessageId",summary->at("endMessageId")},{"endMessageId",summary->at("endMessageId")}});
            const auto status = std::ranges::find_if(turn,[](const auto& item) {
                return item.at("tool") == "web_fetch";
            });
            if (status == turn.end()) return fetch(statusUrl);

            const auto& result = status->at("result");
            check(result.at("ok") == false && !field(result,"error").empty(),"The status fetch unexpectedly succeeded");
            const auto& original = expanded->at("result").at("messages").at(0);
            check(field(original,"text") == "Receipt: "+std::string(receipt),"The original receipt was not restored exactly");
            return reply(historyValue(history,"Audit scope: ")+" | "+lineAfter(field(original,"text"),"Receipt: ")+" | UNVERIFIED");
        },
        [&f] {
            const auto fetched = f.web->fetches.find(statusUrl);
            std::size_t expansions = 0;
            for (const auto& record : f.model.calls)
                expansions += std::ranges::count(record.at("actions"),"expand_context");
            return f.web->fetches.size() == 1 && fetched != f.web->fetches.end() && fetched->second == 1 &&
                expansions == 1;
        }};
}

Json::const_iterator latestFetch(const Json& turn) {
    const auto newest = std::find_if(turn.rbegin(),turn.rend(),[](const auto& entry) {
        return entry.at("tool") == "web_fetch";
    });
    return newest == turn.rend() ? turn.end() : std::prev(newest.base());
}

std::string chainRequest(int series) {
    return "Start at " + chainUrl(series,1) + ". Follow each nextUrl in order, one web_fetch per step, "
        "until nextUrl is empty. Use only the shown URLs. Ignore the rows except for a requested review seal. "
        "Use a saved tool result if an omitted result is needed. Never fetch the same page again.";
}

Case nearLimit(Fixture& f) {
    // Each web_fetch also charges the request budget. The turn still has its normal 24-step cap.
    f.config["modelCallLimit"] = 48;
    return {"near-limit",chainRequest(0) +
        " Reply with only the first and last ticket, separated by ' / '.",
        chainTicket(0,1)+" / "+chainTicket(0,limitPages),
        [](const Json& context) {
            const auto& turn = context.at("event").at("turn");
            if (turn.empty()) return fetch(chainUrl(0,1));
            const auto& data = turn.back().at("result").at("data");
            if (const auto next = field(data,"nextUrl"); !next.empty()) return fetch(next);
            return reply(field(turn.front().at("result").at("data"),"ticket")+" / "+field(data,"ticket"));
        },
        [&f] { return f.web->fetchedChainOnce(0) && f.web->fetches.size() == limitPages &&
            f.model.calls.size() >= 22 && f.model.calls.size() <= 24; }};
}

// The first report's only copy of its terminal ticket is past the old-history preview.
constexpr std::string_view archiveNote =
    "Archive display note. The receiving manifest was checked against the opening inventory. "
    "The reviewer kept the copy supplied with the shipment and recorded each correction in the inventory log. "
    "Use the same order when comparing a later manifest. Begin with the delivery receipt, then confirm the location, "
    "then read the sequence of inventory entries. Save the original receipts with the signed manifest for the next audit. "
    "For any item that changed locations, check both the opening and closing inventory for the same date. "
    "The receiving team will use that record to answer questions about the original manifest.";

Json::const_iterator archivedReply(const Json& history) {
    return std::ranges::find_if(history,[](const auto& entry) {
        return entry.value("role","") == "bot" && field(entry,"text").starts_with("Archive display note.");
    });
}

Decision seriesStep(const Json& context, int series) {
    const auto& event = context.at("event");
    const auto& history = context.at("history");
    const auto& turn = event.at("turn");
    const auto opening = archivedReply(history);
    const auto summary = std::ranges::find_if(history,[](const auto& entry) {
        return entry.value("kind","") == "conversation_compaction";
    });
    if (series == seriesCount) {
        if (turn.empty()) {
            check(opening != history.end() && opening->value("evidenceOmitted",false) &&
                  lineAfter(field(*opening,"text"),"Terminal ticket: ").empty(),"Expected the first reply's evidence preview");
            return action("compact_conversation",{{"endMessageId",opening->at("messageId")},
                {"summary","Initial report prefix was ORCHARD VERIFIED. Later instructions may update it. "
                           "Expand the Bot reply at the endMessageId to read the original terminal ticket."}});
        }
        check(summary != history.end() && opening == history.end(),"Expected the cumulative summary to replace the first exchange");
        if (turn.size() == 1) return action("expand_context",{{"compactionId",summary->at("compactionId")},
            {"startMessageId",summary->at("endMessageId")},{"endMessageId",summary->at("endMessageId")}});
    }

    const auto latest = latestFetch(turn);
    if (latest == turn.end()) return fetch(chainUrl(series,1));
    const auto& terminal = latest->at("result").at("data");
    if (const auto next = field(terminal,"nextUrl"); !next.empty()) return fetch(next);

    auto prefix = lineAfter(field(event,"text"),"Report prefix: ");
    if (prefix.empty()) prefix = historyValue(history,"Report prefix: ");
    const auto ticket = field(terminal,"ticket");
    if (series == 1)
        return reply(lineAfter(field(event,"text"),"Copy this note: ")+"\n"+prefix+"\nTerminal ticket: "+ticket);
    if (series != seriesCount) {
        check(opening != history.end(),"The first report is missing from recent history");
        return reply(prefix+" | "+lineAfter(field(*opening,"text"),"Terminal ticket: ")+" | "+ticket);
    }

    const auto expanded = std::ranges::find_if(turn,[](const auto& entry) {
        return entry.at("tool") == "expand_context";
    });
    check(expanded != turn.end(),"The archived reply must be read before fetching");
    const auto oldTicket = lineAfter(field(expanded->at("result").at("messages").at(0),"text"),"Terminal ticket: ");
    const auto source = std::ranges::find_if(turn,[&](const auto& entry) {
        return entry.at("tool") == "web_fetch" && field(entry.at("args"),"url") == field(terminal,"sealSource");
    });
    check(source != turn.end(),"The seal's source page was not fetched");
    const auto& sourceResult = source->at("result");
    if (turn.back().at("tool") != "read_tool_result" && sourceResult.value("evidenceOmitted",false))
        return action("read_tool_result",sourceResult.at("resultRef"));
    const auto& record = turn.back().at("tool") == "read_tool_result" ?
        turn.back().at("result").at("result") : sourceResult;
    return reply(prefix+" | "+oldTicket+" | "+ticket+" | "+
                 lineAfter(field(record.at("data"),"rows"),"Review seal: "));
}

std::vector<Case> auditSeries(Fixture& f) {
    f.model.allowCompaction = true;
    std::vector<Case> cases;
    const auto oldTicket = chainTicket(1,seriesPages);
    for (int series = 1; series <= seriesCount; ++series) {
        const auto prefix = std::string(series <= 2 ? openingPrefix : revisedPrefix);
        const auto ticket = chainTicket(series,seriesPages);
        auto message = chainRequest(series);
        auto expected = prefix+" | "+oldTicket+" | "+ticket;
        if (series == 1) {
            message += "\nUse this report prefix on later turns unless I update it.\nReport prefix: "+prefix+
                "\nFor this first report, copy the note verbatim onto line 1, write the report prefix on line 2, "
                "and write Terminal ticket: followed by the exact last ticket on line 3.\nCopy this note: "+
                std::string(archiveNote);
            expected = std::string(archiveNote)+"\n"+prefix+"\nTerminal ticket: "+ticket;
        } else if (series == seriesCount) {
            message = "Before fetching, compact the first exchange through the first Bot reply. Keep the "
                "original reporting convention and point to the first Bot reply at the compaction endMessageId, "
                "without putting the ticket in the summary. Use expand_context to read that original Bot reply "
                "by its exact message ID. Then proceed with the new manifest. "+message+
                " The terminal result identifies sealSource, an earlier page in this same turn. Read its "
                "'Review seal' in rows. If omitted, use read_tool_result with its saved resultRef. "
                "Reply with just the latest report prefix, the terminal ticket from the original Bot reply, "
                "the new terminal ticket, and the review seal, separated by ' | '.";
            expected += " | "+std::string(reviewSeal);
        } else {
            message += " Reply with only the current report prefix, the first audit's terminal ticket from "
                "our earliest Bot reply, and the new terminal ticket, separated by ' | '.";
            if (series == 3)
                message += "\nUse this report prefix from now on.\nReport prefix: "+prefix;
        }
        cases.push_back({std::to_string(series),std::move(message),std::move(expected),
            [series](const Json& context) { return seriesStep(context,series); },
            [&f,series] {
                if (!f.web->fetchedChainOnce(series) || f.web->fetches.size() != std::size_t(series*seriesPages))
                    return false;
                if (series != seriesCount) return true;
                const auto summaries = f.store.compactions("audit");
                if (summaries.size() != 1 || field(summaries.front(),"summary").find(chainTicket(1,seriesPages)) !=
                    std::string::npos) return false;
                return std::ranges::any_of(f.model.calls,[](const auto& call) {
                    return call.at("turnIndex") == seriesCount && call.at("firstInTurn") == true &&
                        call.at("omittedHistoryEntries") > 0 && call.at("compactionId").is_null();
                }) && std::ranges::any_of(f.model.calls,[](const auto& call) {
                    return !call.at("compactionId").is_null();
                }) && f.model.calls.size() >= 30 && f.model.calls.size() <= 60;
            }});
    }
    return cases;
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

void addTotals(Json& result, const Json& calls) {
    std::size_t evidenceReads = 0;
    for (const auto& call : calls) {
        for (const auto& action : call.at("actions"))
            if (action == "expand_context" || action == "read_tool_result") ++evidenceReads;
    }
    result["callCount"] = calls.size();
    result["totalLocalRequestBytes"] = totalUsage(calls,"localRequestBytes");
    result["totalTransportRequestBytes"] = totalUsage(calls,"transportRequestBytes");
    result["totalProviderInputTokens"] = totalUsage(calls,"providerInputTokens");
    result["totalProviderCachedInputTokens"] = totalUsage(calls,"providerCachedInputTokens");
    result["totalProviderUncachedInputTokens"] = totalUsage(calls,"providerUncachedInputTokens");
    result["totalProviderCacheWriteTokens"] = totalUsage(calls,"providerCacheWriteTokens");
    result["evidenceRereads"] = evidenceReads;
}

bool measure(Fixture& fixture, std::string_view name, const std::vector<Case>& tests, std::string_view slug, bool live) {
    const auto begin = Clock::now();
    auto turns = Json::array();
    bool success = true;
    fixture.model.measuring = true;
    for (const auto& test : tests) {
        fixture.model.script = test.script;
        const auto firstCall = fixture.model.calls.size();
        const auto turnBegin = Clock::now();
        const auto message = fixture.send(test.message);
        const auto answer = message.value("reply",Json::object());
        const auto actual = field(answer,"text");
        const auto passed = field(answer,"route") == "reply" && actual == test.expected && test.evidenceChecked();
        success = success && passed;
        auto& row = turns.emplace_back(Json{{"turnIndex",fixture.model.turnIndex},{"wallMs",elapsedMs(turnBegin)},
            {"success",passed},{"reply",actual},{"expected",test.expected}});
        auto calls = Json::array();
        for (auto index = firstCall; index < fixture.model.calls.size(); ++index)
            calls.push_back(fixture.model.calls.at(index));
        addTotals(row,calls);
    }
    Json result{{"case",name},{"mode",live ? "live" : "fixed"},{"model",slug},
        {"calls",fixture.model.calls},{"turns",turns},{"wallMs",elapsedMs(begin)},{"success",success},
        {"webFetches",fixture.web->fetches}};
    addTotals(result,fixture.model.calls);
    if (tests.size() == 1) {
        result["reply"] = turns.at(0).at("reply");
        result["expected"] = turns.at(0).at("expected");
    }
    std::cout << result.dump() << std::endl;
    return success;
}
}

int main(int argc, char** argv) {
    try {
        const bool baseline = argc == 2 && std::string_view(argv[1]) == "--measure-baseline";
        const bool fixed = argc == 3 && std::string_view(argv[1]) == "--fixed";
        const bool live = argc == 5 && std::string_view(argv[1]) == "--live";
        check(argc == 1 || baseline || fixed || live,
              "Usage: flock_model_context [--measure-baseline | --fixed CASE | --live PROFILE_DIR MODEL CASE]");
        const std::string_view slug = live ? argv[3] : "gpt-6.2-sol";
        const std::string_view selected = live ? argv[4] : fixed ? argv[2] : "all";
        std::optional<app::ChatGPTModel> provider;
        if (live) provider.emplace(argv[2]);
        auto* adapter = live ? &*provider : nullptr;
        bool success = true, matched = false;
        const auto run = [&](std::string_view name, const auto& build) {
            if (selected != "all" && selected != name) return;
            matched = true;
            Fixture fixture(slug,adapter);
            auto cases = build(fixture);
            if constexpr (std::is_same_v<decltype(cases),Case>)
                success = measure(fixture,name,{std::move(cases)},slug,live) && success;
            else success = measure(fixture,name,cases,slug,live) && success;
        };
        run("short",[](auto&) { return shortRequest(); });
        run("long-followup",longFollowup);
        run("large-results",[&](auto& fixture) { return largeToolResults(fixture,!baseline && !live); });
        run("old-reference",oldReference);
        run("quality-replay",qualityReplay);
        run("near-limit",nearLimit);
        run("multi-turn",auditSeries);
        check(matched,"Unknown fixture case");
        return success ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
