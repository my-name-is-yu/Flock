#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"

#ifdef FLOCK_WITH_T3
#include "flock/t3_mcp.hpp"
#endif
#ifdef FLOCK_WITH_WEB
#include "flock/web.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace flockv0::app {
namespace {
constexpr std::string_view usage = R"(Usage: flock [--home DIRECTORY] COMMAND

  run [--fixed] [--no-web | --headless] [--port PORT] [--session-read off|on_demand|prefill]
  bots
  submit BOT REQUEST
  tasks [BOT] [--before ID] [--limit N]
  status TASK
  history TASK [--after ID] [--limit N]
  answer TASK WAIT TEXT
  approve TASK WAIT
  retry TASK WAIT
  extend TASK WAIT NEW_UNIX_DEADLINE
  reconcile TASK WAIT [--thread ID] [--run ID] [--question ID]
  cancel TASK [WAIT]
  t3 login ENVIRONMENT MCP_URL
  model login [--print-url] [--reconsent]
  model status
  model models
  model acceptance MODEL_SLUG

State defaults to $FLOCK_HOME or ~/.config/flock. Quote requests and answers
containing spaces. Run --fixed uses local demo adapters for both model and T3.
)";

class Arguments {
public:
    Arguments(int argc, char** argv) : remaining_(argc-1), next_(argv+1) {}
    bool empty() const { return remaining_ == 0; }
    std::string_view peek() const { return empty() ? std::string_view{} : *next_; }
    std::string pop(std::string_view what) {
        if (empty()) throw Conflict("Expected " + std::string(what));
        --remaining_;
        return *next_++;
    }
    void end() const {
        if (!empty()) throw Conflict("Unexpected argument: " + std::string(peek()));
    }
private:
    int remaining_;
    char** next_;
};

Id integer(std::string_view input, std::string_view name, Id lower = 1, Id upper = INT64_MAX) {
    Id value = 0;
    const auto [end,error] = std::from_chars(input.data(),input.data()+input.size(),value);
    if (error != std::errc{} || end != input.data()+input.size() || value < lower || value > upper)
        throw Conflict("Invalid " + std::string(name));
    return value;
}

Id number(Arguments& args, std::string_view name, Id lower = 1, Id upper = INT64_MAX) {
    return integer(args.pop(name),name,lower,upper);
}

std::filesystem::path homePath(Arguments& args) {
    if (args.peek() == "--home") {
        args.pop("--home");
        const auto path = args.pop("Flock home");
        if (path.empty()) throw Conflict("Flock home must not be empty");
        return std::filesystem::absolute(path);
    }
    if (const auto* configured = std::getenv("FLOCK_HOME"); configured && *configured)
        return std::filesystem::absolute(configured);
    if (const auto* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / "flock";
    throw Conflict("Set FLOCK_HOME or provide --home DIRECTORY");
}

void print(const Json& value) {
    std::cout << value.dump(2,' ',false,Json::error_handler_t::replace) << '\n';
}

int modelCommand(Arguments& args, const std::filesystem::path& home) {
    ChatGPTModel model(home / "credentials" / "chatgpt");
    const auto action = args.pop("model command");
    if (action == "login") {
        bool printUrl = false, reconsent = false;
        while (!args.empty()) {
            const auto option = args.pop("login option");
            if (option == "--print-url") printUrl = true;
            else if (option == "--reconsent") reconsent = true;
            else throw Conflict("Unknown model login option: " + option);
        }
        const auto result = model.login(printUrl,reconsent);
        print(result);
        if (!result.value("planUsage",false)) {
            std::cerr << "Use 'flock model login --reconsent' to grant ChatGPT plan usage.\n";
            return 1;
        }
    } else if (action == "status") {
        args.end();
        print(model.status());
    } else if (action == "models") {
        args.end();
        print(model.models());
    } else if (action == "acceptance") {
        const auto slug = args.pop("model slug");
        args.end();
        return runModelAcceptance(model,slug,std::cout) ? 0 : 1;
    } else throw Conflict("Unknown model command: " + action);
    return 0;
}

int t3Command(Arguments& args, const std::filesystem::path& home) {
    if (args.pop("T3 command") != "login") throw Conflict("Unknown T3 command");
    const auto environment = args.pop("canonical environment ID");
    const auto url = args.pop("T3 MCP URL");
    args.end();
#ifdef FLOCK_WITH_T3
    loginT3(home / "credentials" / "t3",environment,url,[](const std::string& authorization) {
        std::cerr << "Open this URL in your browser to authorize Flock's T3 connection.\n" << authorization << '\n';
    });
    print({{"environmentId",environment},{"connected",true}});
    return 0;
#else
    (void)home;
    throw Conflict("This build does not include the T3 adapter");
#endif
}

volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }

void daemon(Arguments& args, const std::filesystem::path& home, Store& store, Bots& bots) {
    bool fixed = false, web = true;
    [[maybe_unused]] unsigned port = 0;
    RuntimeOptions options;
    while (!args.empty()) {
        const auto option = args.pop("run option");
        if (option == "--fixed") fixed = true;
        else if (option == "--no-web" || option == "--headless") web = false;
        else if (option == "--port") port = static_cast<unsigned>(number(args,"port",0,65535));
        else if (option == "--session-read") {
            options.sessionRead = args.pop("session read mode");
            if (options.sessionRead != "off" && options.sessionRead != "on_demand" && options.sessionRead != "prefill")
                throw Conflict("Session read mode must be off, on_demand or prefill");
        } else throw Conflict("Unknown run option: " + option);
    }
#ifndef FLOCK_WITH_WEB
    if (web) throw Conflict("This build does not include the Web UI. Run with --no-web.");
#endif
#ifndef FLOCK_WITH_T3
    if (!fixed) throw Conflict("This build does not include the T3 adapter. Use --fixed for the local demo.");
#endif
    std::unique_ptr<Model> model;
    std::unique_ptr<T3> t3;
    if (fixed) {
        model = makeDemoModel();
        t3 = makeDemoT3();
    }
#ifdef FLOCK_WITH_T3
    else {
        model = std::make_unique<ChatGPTModel>(home / "credentials" / "chatgpt");
        t3 = makeT3Mcp(home / "credentials" / "t3");
    }
#else
    (void)home;
#endif
    const auto startedAt = unixTime();
    options.peers = [&](std::string_view botId) { return bots.peers(botId); };
    Runtime runtime(store,*model,*t3,[&](std::string_view botId) { return bots.notes(botId); },options);
#ifdef FLOCK_WITH_WEB
    std::unique_ptr<WebServer> server;
    if (web) {
        server = startWeb(store,port,{
            [&] { return bots.cards(); },
            [&](std::string_view botId,std::string_view request) { return bots.get(botId).intake(request); },
            [&](const Json& profile,const std::optional<AvatarImage>& image) { return bots.create(profile,image); },
            [&](std::string_view botId,const Json& profile,const std::optional<AvatarImage>& image) {
                return bots.updateProfile(botId,profile,image);
            },
            [&](std::string_view botId) { return bots.avatarImage(botId); },
            options.peers,
        });
        std::cout << "Flock Web UI at http://127.0.0.1:" << server->port() << std::endl;
    }
#endif
    std::signal(SIGINT,requestStop);
    std::signal(SIGTERM,requestStop);
    if (!web) std::cout << "Flock Runtime is running in headless mode." << std::endl;

    Scheduler schedule(bots,store,startedAt,[](std::string_view id,std::string_view error) {
        std::cerr << "Flock Bot " << id << ": " << error << '\n';
    });
    while (!stopRequested) {
        try { schedule.tick(unixTime()); }
        catch (const std::exception& error) {
            std::cerr << "Unable to submit scheduled tasks: " << error.what() << '\n';
        }
        runtime.tickConcurrent();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void operate(Store& store, const Bots& bots, const std::string& operation, Arguments& args) {
    const auto task = number(args,"task ID");
    Id wait = 0;
    Json details = Json::object();
    if (operation != "cancel" || !args.empty()) wait = number(args,"wait ID");
    if (operation == "answer") details["text"] = args.pop("answer");
    else if (operation == "extend") details["deadline"] = number(args,"new Unix deadline");
    else if (operation == "reconcile") {
        while (!args.empty()) {
            const auto flag = args.pop("reconciliation option");
            if (flag == "--thread") details["threadId"] = args.pop("Thread ID");
            else if (flag == "--run") details["runId"] = args.pop("Run ID");
            else if (flag == "--question") details["questionId"] = args.pop("question ID");
            else throw Conflict("Unknown reconciliation option: " + flag);
        }
        const auto status = store.task(task);
        if (status["wait"].is_object() && status["wait"]["id"] == wait && status["wait"]["reason"] == "settings" &&
            (status["pendingAction"].is_null() || status["pendingAction"]["phase"] == "ready")) {
            const auto bot = bots.get(status["botId"].get<std::string>());
            const auto destinations = bot.settings.value("conversationEnvironments",Json::object());
            const auto& environment = status.at("environmentId");
            const bool allowed = environment == bot.environmentId ||
                std::ranges::find(destinations,environment) != destinations.end();
            if (!allowed || status["projectId"] != Json(bot.projectId))
                throw Conflict("The project route changed. Cancel this task and submit a new request.");
            details["session"] = bot.settings.value("session",Json::object());
        }
    }
    args.end();
    store.operate(task,wait,operation,details);
    print(store.task(task));
}

void taskCommand(Store& store, const std::string& command, Arguments& args) {
    Id task = 0, cursor = 0;
    unsigned limit = 50;
    std::string bot;
    if (command == "history") task = number(args,"task ID");
    else if (!args.empty() && !args.peek().starts_with("--")) bot = args.pop("Bot ID");
    while (!args.empty()) {
        const auto flag = args.pop("pagination option");
        if ((command == "history" && flag == "--after") || (command == "tasks" && flag == "--before"))
            cursor = number(args,"cursor",0);
        else if (flag == "--limit") limit = static_cast<unsigned>(number(args,"limit",1,200));
        else throw Conflict("Unknown pagination option: " + flag);
    }
    if (command == "history") {
        if (!store.hasTask(task)) throw Conflict("Task does not exist");
        print(store.history(task,cursor,limit));
    } else print(store.tasks(bot,cursor,limit));
}

int runCli(Arguments& args) {
    if (args.empty() || args.peek() == "--help" || args.peek() == "-h") {
        std::cout << usage;
        return 0;
    }
    const auto home = homePath(args);
    const auto command = args.pop("command");
    if (command == "--help" || command == "-h") {
        std::cout << usage;
        return 0;
    }
    if (command == "model") return modelCommand(args,home);
    if (command == "t3") return t3Command(args,home);
    Bots bots(home / "bots");
    if (command == "bots") {
        args.end();
        print(bots.cards());
        return 0;
    }
    std::filesystem::create_directories(home);
    Store store(home / "flock.sqlite3");
    if (command == "run") daemon(args,home,store,bots);
    else if (command == "submit") {
        const auto bot = bots.get(args.pop("Bot ID"));
        const auto request = args.pop("request");
        args.end();
        const auto id = store.submit(bot.intake(request));
        print(store.task(id));
    } else if (command == "status") {
        const auto id = number(args,"task ID");
        args.end();
        print(store.task(id));
    } else if (command == "history" || command == "tasks") taskCommand(store,command,args);
    else if (command == "answer" || command == "approve" || command == "retry" || command == "extend" ||
             command == "reconcile" || command == "cancel") operate(store,bots,command,args);
    else throw Conflict("Unknown command: " + command + ". Run 'flock --help'.");
    return 0;
}
}
}

int main(int argc,char** argv) {
    try {
        flockv0::app::Arguments args(argc,argv);
        return flockv0::app::runCli(args);
    } catch (const std::exception& error) {
        std::cerr << "flock: " << error.what() << '\n';
        return 1;
    }
}
