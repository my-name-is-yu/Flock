#include "bot_tools.hpp"
#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"
#include "git_github.hpp"
#include "t3_setup.hpp"
#include "flock/t3_connection.hpp"
#include "flock/identity.hpp"

#include "flock/root_delivery.hpp"
#include "flock/session_handoff.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/web.hpp"

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <set>
#include <thread>

namespace flockv0::app {
namespace {
constexpr std::string_view usage = R"(Usage: flock [--home DIRECTORY] COMMAND

  run [--fixed] [--no-web] [--port PORT] [--session-read off|on_demand|prefill]
  bots
  access grant BOT KIND RESOURCE ACTION
  access revoke BOT KIND RESOURCE ACTION
  access list BOT
  submit BOT REQUEST
  tasks [BOT] [--before ID] [--limit N]
  status TASK
  history TASK [--after ID] [--limit N]
  answer TASK WAIT REVISION GENERATION TEXT
  approve TASK WAIT REVISION GENERATION
  retry TASK WAIT REVISION GENERATION
  extend TASK WAIT REVISION GENERATION NEW_UNIX_DEADLINE
  reconcile TASK WAIT REVISION GENERATION [--thread ID] [--run ID] [--question ID]
  cancel TASK [WAIT REVISION GENERATION]
  t3 connect ENVIRONMENT MCP_URL [--mode full|consult] [--port PORT]
  t3 disconnect ENVIRONMENT
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

Store openStore(const std::filesystem::path& home) {
    return Store(home / "flock.sqlite3");
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

int t3Command(Arguments& args, const std::filesystem::path& home, Bots& bots) {
    const auto action = args.pop("T3 command");
    if (action != "connect" && action != "disconnect") throw Conflict("Unknown T3 command");
    const auto environment = args.pop("canonical environment ID");
    if (action == "disconnect") {
        args.end();
        const T3SetupLock setup;
        auto store = openStore(home);
        T3Connections connections(store);
        if (connections.disconnect(environment)) {
            removeT3Providers([&](std::string_view token) { return connections.ownsBearer(token); });
            connections.providerRemoved();
        }
        print({{"environmentId",environment},{"connected",false}});
        return 0;
    }
    const auto url = args.pop("T3 MCP URL");
    std::optional<T3ConnectionMode> mode;
    unsigned port = 0;
    while (!args.empty()) {
        const auto flag = args.pop("T3 connection option");
        if (flag == "--port") port = static_cast<unsigned>(number(args,"port",1,65535));
        else if (flag == "--mode") {
            const auto value = args.pop("connection mode");
            if (value == "consult") mode = T3ConnectionMode::Consult;
            else if (value == "full") mode = T3ConnectionMode::Full;
            else throw Conflict("T3 connection mode must be full or consult");
        } else throw Conflict("Unknown T3 connection option: " + flag);
    }
    const T3SetupLock setup;
    auto store = openStore(home);
    if (const auto identities = std::getenv("FLOCK_WEB_IDENTITIES_FILE")) {
        WebCallbacks callbacks;
        configureWebIdentities(store,identities,callbacks);
        if (!callbacks.configuredPerson(localOwner.id))
            throw Conflict("Configure a Web identity for local-owner before connecting T3");
    } else if (store.access().requiresIdentity()) throw Conflict("Set FLOCK_WEB_IDENTITIES_FILE before connecting T3");
    T3Connections connections(store);
    const auto previous = connections.settings(localOwner.id);
    const auto savedPort = previous ? previous->port : 0;
    if (!port) port = savedPort ? savedPort : 4173;
    const auto selectedMode = mode.value_or(previous ? previous->mode : T3ConnectionMode::Full);
    const auto providers = configureT3Providers("http://127.0.0.1:" + std::to_string(port) + "/mcp/flock",
        [&](const InstallT3Provider& install) {
            loginT3(home / "credentials" / "t3",environment,url,[](const std::string& authorization) {
                std::cerr << "Open this URL in your browser to authorize Flock's T3 connection.\n" << authorization << '\n';
            },true,[&](const std::function<void()>& saveCredential) {
                const auto refreshProviders = t3ProvidersNeedRefresh([&](std::string_view token) {
                    return connections.ownsBearer(token);
                });
                connections.connect(environment,localOwner.id,selectedMode,port,[&](std::string_view token) {
                    if (!token.empty()) install(token);
                    saveCredential();
                },refreshProviders);
            });
        });
    if (savedPort != port)
        std::cerr << "T3 skills use Flock Web port " << port << ". Restart 'flock run' if it is already running.\n";
    try { syncT3Skills(bots.skillCards()); }
    catch (const std::exception& error) {
        std::cerr << "Could not export T3 skills: " << error.what() << '\n';
    }
    print({{"environmentId",environment},{"connected",true},{"mode",t3ConnectionModeName(selectedMode)},
           {"port",port},{"configuredProviders",providers}});
    return 0;
}

volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }

void daemon(Arguments& args, const std::filesystem::path& home, Bots& bots) {
    bool fixed = false, web = true;
    std::optional<unsigned> port;
    RuntimeOptions options;
    while (!args.empty()) {
        const auto option = args.pop("run option");
        if (option == "--fixed") fixed = true;
        else if (option == "--no-web") web = false;
        else if (option == "--port") port = static_cast<unsigned>(number(args,"port",0,65535));
        else if (option == "--session-read") {
            options.sessionRead = args.pop("session read mode");
            if (options.sessionRead != "off" && options.sessionRead != "on_demand" && options.sessionRead != "prefill")
                throw Conflict("Session read mode must be off, on_demand or prefill");
        } else throw Conflict("Unknown run option: " + option);
    }
    Store store(home / "flock.sqlite3",[&](std::string_view id) { return bots.initialMemory(id); });
    const auto savedPort = T3Connections(store).port();
    if (savedPort && !web)
        std::cerr << "Warning: T3 skills cannot reach Flock in headless mode. Start 'flock run' with Web enabled.\n";
    if (savedPort && web && port && *port != savedPort)
        throw Conflict("T3 skills use the saved Web port " + std::to_string(savedPort) +
                       ". Omit --port or change the saved port with 'flock t3 connect'.");
    if (const auto* token = std::getenv("FLOCK_GITHUB_TOKEN"); token && *token) {
        options.gitHost = makeGitHubHost(token);
        store.protectGitCredential(token);
    }
    std::unique_ptr<Model> model;
    std::unique_ptr<T3> t3;
    if (fixed) {
        model = makeDemoModel();
        t3 = makeDemoT3();
    } else {
        model = std::make_unique<ChatGPTModel>(home / "credentials" / "chatgpt");
        t3 = makeT3Mcp(home / "credentials" / "t3");
    }
    options.botTools = loadBotTools(home);
    options.peers = [&](std::string_view botId) { return bots.peers(botId); };
    options.automationError = [](std::string_view error) { std::cerr << "Unable to fire automations: " << error << '\n'; };
    options.storageContention = [](std::string_view error) { std::cerr << "Flock storage busy: " << error << '\n'; };
    const IntakeResolver intake = [&](std::string_view botId, std::string_view request) {
        return bots.get(botId).intake(request);
    };
    const Runtime::SoulLoader soul = [&](std::string_view botId) { return bots.soul(botId); };
    const auto identityFile = std::getenv("FLOCK_WEB_IDENTITIES_FILE");
    auto identities = identityFile ? loadIdentities(store,identityFile) : ConfiguredIdentities{};
    if (!identityFile && !store.access().requiresIdentity()) {
        identities.resolvePerson = [&store](std::string_view id) -> std::optional<Person> {
            if (id == localOwner.id && !store.access().requiresIdentity()) return localOwner;
            return std::nullopt;
        };
    }
    auto handoffSession = !fixed && identities.resolvePerson
        ? makeT3ThreadSession(home / "credentials" / "t3") : nullptr;
    std::shared_ptr<SessionHandoff> handoffs;
    if (!fixed) {
        options.resolveSpecialistPerson = [&](std::string_view person) -> std::optional<Person> {
            return identities.resolvePerson ? identities.resolvePerson(person) : std::nullopt;
        };
    }
    if (handoffSession) {
        options.observeSpecialistInput = [&](const Person& person, Id channel) -> std::optional<std::string> {
            if (!handoffs) return {};
            const auto view = handoffs->view(person,channel);
            if (!view.coordination) return {};
            return handoffSession->inputCommit(view.origin.invitation.target);
        };
    }
    Runtime runtime(store,*model,*t3,soul,options);
    bots.syncSchedules(store,options.clock(),[](std::string_view id,std::string_view error) {
        std::cerr << "Flock Bot " << id << ": " << error << '\n';
    });
    if (handoffSession) {
        handoffs = std::make_shared<SessionHandoff>(store,*handoffSession,identities.resolvePerson,intake,&runtime,options.peers);
        handoffs->recover();
    }
    std::jthread handoffWorker;
    Json exportedBots;
    auto refreshSkills = [&] {
        try {
            auto descriptions = bots.skillCards();
            if (descriptions != exportedBots) {
                syncT3Skills(descriptions);
                exportedBots = std::move(descriptions);
            }
        } catch (const std::exception& error) {
            std::cerr << "Could not refresh T3 skills: " << error.what() << '\n';
        }
    };
    // Web callbacks and the filesystem refresh both update the same on-disk skills.
    std::mutex skillMutex;
    auto syncSkills = [&] {
        if (!T3Connections(store).configured()) return;
        std::lock_guard lock(skillMutex);
        refreshSkills();
    };
    if (web) syncSkills();
    WebCallbacks callbacks{
        [&] { return bots.cards(); },
        intake,
        [&](const Json& profile,const std::optional<AvatarImage>& image) {
            auto created = bots.create(profile,image);
            syncSkills();
            return created;
        },
        [&](std::string_view botId,const Json& profile,const std::optional<AvatarImage>& image) {
            auto updated = bots.updateProfile(botId,profile,image);
            syncSkills();
            return updated;
        },
        [&](std::string_view botId) { return bots.avatarImage(botId); },
        options.peers,
        identities.authenticate,
        [&](const BotChange& change) { bots.apply(change); syncSkills(); },
        options.clock,
    };
    callbacks.resolveIdentity = identities.resolveRoot;
    const auto bridges = std::getenv("FLOCK_CHANNEL_BRIDGES_FILE");
    if (bridges) configureChannelBridges(store,bridges,callbacks);
    callbacks.sessionHandoffs = handoffs;
    std::unique_ptr<WebServer> server;
    if (web) {
        if (const auto* secret = std::getenv("FLOCK_GITHUB_WEBHOOK_SECRET")) callbacks.githubWebhookSecret = secret;
        server = startWeb(store,port.value_or(savedPort),std::move(callbacks));
        std::cout << "Flock Web UI at http://127.0.0.1:" << server->port() << std::endl;
    }
    std::signal(SIGINT,requestStop);
    std::signal(SIGTERM,requestStop);
    if (handoffs) {
        handoffWorker = std::jthread([&](std::stop_token stop) {
            Id after = 0;
            bool pollingFailed = false;
            std::set<Id> reportedFailures;
            while (!stop.stop_requested()) {
                try {
                    const auto pending = handoffs->pending(after);
                    pollingFailed = false;
                    if (pending.empty()) after = 0;
                    for (const auto& [channel, personId] : pending) {
                        if (stop.stop_requested()) return;
                        after = channel;
                        try {
                            const auto actor = identities.resolvePerson(personId);
                            if (!actor) {
                                if (reportedFailures.insert(channel).second)
                                    std::cerr << "Session handoff " << channel << " has no configured identity.\n";
                                continue;
                            }
                            const auto view = handoffs->view(*actor,channel);
                            if (view.coordination) handoffs->step(*actor,channel,*model,soul);
                            else handoffs->consult(*actor,channel,*model,soul);
                            reportedFailures.erase(channel);
                        } catch (const std::exception&) {
                            if (reportedFailures.insert(channel).second)
                                std::cerr << "Could not advance Session handoff " << channel
                                          << ". Check its access and Bot settings.\n";
                        }
                    }
                } catch (const std::exception&) {
                    if (!pollingFailed) std::cerr << "Could not load pending Session handoffs.\n";
                    pollingFailed = true;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });
    }
    if (!web) std::cout << "Flock Runtime is running in headless mode." << std::endl;

    auto nextSkillRefresh = std::chrono::steady_clock::now();
    while (!stopRequested) {
        if (web && std::chrono::steady_clock::now() >= nextSkillRefresh) {
            syncSkills();
            nextSkillRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        runtime.tickConcurrent();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    handoffWorker.request_stop();
    runtime.requestStop();
}

void operate(const std::filesystem::path& home, const Bots& bots, const std::string& operation, Arguments& args) {
    const auto task = number(args,"task ID");
    auto store = openStore(home);
    const auto status = store.task(task);
    auto target = TaskTarget::from(status,0);
    if (operation != "cancel" || !args.empty()) {
        target.waitId = number(args,"wait ID");
        target.revision = number(args,"revision ID",0);
        target.generation = number(args,"generation",0);
    }
    Json details = Json::object();
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
    }
    args.end();

    if (operation == "reconcile") {
        details = reconciliationData(status,target.waitId,std::move(details),[&](std::string_view id, std::string_view request) {
            return bots.get(id).intake(request);
        });
    }
    store.operate(localOwner,target,operation,details);
    print(store.task(task));
}

void taskCommand(const std::filesystem::path& home, const std::string& command, Arguments& args) {
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
    const auto store = openStore(home);
    if (command == "history") {
        if (!store.hasTask(task)) throw Conflict("Task does not exist");
        print(store.history(task,cursor,limit));
    } else print(store.tasks(bot,cursor,limit));
}

void accessCommand(const std::filesystem::path& home, Arguments& args) {
    const auto action = args.pop("access command (grant, revoke or list)");
    if (action != "grant" && action != "revoke" && action != "list")
        throw Conflict("Unknown access command: " + action);
    const auto bot = args.pop("Bot ID");
    if (!validBotId(bot)) throw Conflict("Invalid Bot ID");
    std::optional<Scope> scope;
    if (action != "list") {
        scope = Scope{args.pop("resource kind"),args.pop("resource ID"),args.pop("resource action")};
        if (!scope->valid()) throw Conflict("Invalid access scope. Check KIND, RESOURCE and ACTION.");
    }
    args.end();
    auto store = openStore(home);
    auto access = store.access();
    if (scope)
        access.resourceGrant(Person{"local-cli",true},bot,*scope,action == "grant" ? GrantChange::grant : GrantChange::revoke);
    print(access.grants(bot));
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
    Bots bots(home / "bots");
    if (command == "t3") return t3Command(args,home,bots);
    if (command == "bots") {
        args.end();
        print(bots.cards());
    } else if (command == "run") daemon(args,home,bots);
    else if (command == "access") accessCommand(home,args);
    else if (command == "submit") {
        const auto botId = args.pop("Bot ID");
        const auto request = args.pop("request");
        args.end();
        const auto intake = bots.get(botId).intake(request);
        auto store = openStore(home);
        const auto id = store.submit(intake);
        print(store.task(id));
    } else if (command == "status") {
        const auto id = number(args,"task ID");
        args.end();
        print(openStore(home).task(id));
    } else if (command == "history" || command == "tasks") taskCommand(home,command,args);
    else if (command == "answer" || command == "approve" || command == "retry" || command == "extend" ||
             command == "reconcile" || command == "cancel") operate(home,bots,command,args);
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
