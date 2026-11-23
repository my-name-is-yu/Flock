#include "bots.hpp"
#include "demo.hpp"
#include "model.hpp"
#include "git_github.hpp"
#include "settings_access.hpp"
#include "settings_connections.hpp"
#include "settings_system.hpp"
#include "t3_setup.hpp"
#include "flock/board_inbox.hpp"
#include "flock/t3_connection.hpp"
#include "flock/identity.hpp"

#include "flock/t3_mcp.hpp"
#include "flock/web.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

namespace flockv0::app {
namespace {
constexpr std::string_view usage = R"(Usage: flock [--home DIRECTORY] COMMAND

  run [--fixed] [--no-web] [--port PORT] [--session-read off|on_demand|prefill]
  bots
  rules list
  mode [full|supervised]
  rules add allow|ask|never ACTION TARGET
  rules remove allow|ask|never ACTION TARGET
  message BOT TEXT
  tasks [BOT] [--before ID] [--limit N]
  status TASK
  history TASK [--after ID] [--limit N]
  answer TASK WAIT REVISION GENERATION TEXT
  approve TASK WAIT REVISION GENERATION
  retry TASK WAIT REVISION GENERATION
  extend TASK WAIT REVISION GENERATION NEW_UNIX_DEADLINE
  reconcile TASK WAIT REVISION GENERATION [--thread ID] [--run ID] [--question ID]
  cancel TASK [WAIT REVISION GENERATION]
  t3 connect ENVIRONMENT MCP_URL [--port PORT]
  t3 disconnect ENVIRONMENT
  model login [--print-url] [--reconsent]
  model status
  model models
  model acceptance MODEL_SLUG

State defaults to $FLOCK_HOME or ~/.config/flock. Quote messages and answers
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

Json activeBotCards(const Policy& policy, Json cards) {
    cards.erase(std::remove_if(cards.begin(),cards.end(),[&](const Json& card) {
        return policy.botDeleted(card.at("id").get_ref<const std::string&>());
    }),cards.end());
    return cards;
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

Json connectT3(const std::filesystem::path& home, const std::optional<std::string>& environment,
               const std::string& url, unsigned port, const std::function<void(const std::string&)>& authorize,
               std::stop_token stop = {}, const std::optional<Person>& requester = {}) {
    const T3SetupLock setup;
    // Provider registration runs inside the connection transaction. Use its own Store
    // so the daemon's reads do not wait on the transaction's Store mutex.
    auto store = openStore(home);
    SettingsAccess access(store,home,std::getenv("FLOCK_WEB_IDENTITIES_FILE"));
    const auto resolve = access.identities().resolvePerson;
    const auto verifyAccess = [&] {
        if (access.enabled() && !resolve(localOwner.id))
            throw Conflict("Keep a Web identity for local-owner to connect T3.");
        if (requester && !access.canManageInstallation(*requester))
            throw Conflict("The account that started this connection can no longer manage Flock.");
    };
    verifyAccess();
    T3Connections connections(store);
    const auto previous = connections.settings(localOwner.id);
    const auto savedPort = previous ? previous->port : 0;
    const auto requestedPort = port;
    const auto cliPort = [&] {
        return SettingsSystem(home,std::nullopt,std::nullopt).t3CliPort(requestedPort,savedPort);
    };
    if (!requester) port = cliPort();
    const auto providers = configureT3Providers("http://127.0.0.1:" + std::to_string(port) + "/mcp/flock",
        [&](const InstallT3Provider& install) {
            loginT3Environment(home / "credentials" / "t3",environment,url,authorize,true,[&](
                const std::string& verifiedEnvironment, const std::function<void()>& saveCredential) {
                const auto refreshProviders = t3ProvidersNeedRefresh([&](std::string_view token) {
                    return connections.ownsBearer(token);
                });
                connections.connect(verifiedEnvironment,localOwner.id,port,[&](std::string_view token) {
                    verifyAccess();
                    if (!requester && cliPort() != port)
                        throw Conflict("The Web port changed during T3 setup. Start the connection again.");
                    if (!token.empty()) install(token);
                    saveCredential();
                },refreshProviders);
            },stop);
        });
    return {{"port",port},{"portChanged",savedPort != port},{"configuredProviders",providers}};
}

void disconnectT3(Store& store, const std::optional<std::string>& environment) {
    const T3SetupLock setup;
    T3Connections connections(store);
    const bool cleanup = environment ? connections.disconnect(*environment) : connections.providerCleanupPending();
    if (cleanup) {
        removeT3Providers([&](std::string_view token) { return connections.ownsBearer(token); });
        connections.providerRemoved();
    }
}

int t3Command(Arguments& args, const std::filesystem::path& home, Bots& bots) {
    const auto action = args.pop("T3 command");
    if (action != "connect" && action != "disconnect") throw Conflict("Unknown T3 command");
    const auto environment = args.pop("canonical environment ID");
    if (action == "disconnect") {
        args.end();
        auto store = openStore(home);
        disconnectT3(store,environment);
        print({{"environmentId",environment},{"connected",false}});
        return 0;
    }
    const auto url = args.pop("T3 MCP URL");
    unsigned port = 0;
    while (!args.empty()) {
        const auto flag = args.pop("T3 connection option");
        if (flag == "--port") port = static_cast<unsigned>(number(args,"port",1,65535));
        else throw Conflict("Unknown T3 connection option: " + flag);
    }
    const auto result = connectT3(home,environment,url,port,[](const std::string& authorization) {
        std::cerr << "Open this URL in your browser to authorize Flock's T3 connection.\n" << authorization << '\n';
    });
    if (result.at("portChanged").get<bool>())
        std::cerr << "T3 skills use Flock Web port " << result.at("port").get<unsigned>()
                  << ". Restart 'flock run' if it is already running.\n";
    try { syncT3Skills(activeBotCards(openStore(home).policy(),bots.skillCards())); }
    catch (const std::exception& error) {
        std::cerr << "Could not export T3 skills: " << error.what() << '\n';
    }
    print({{"environmentId",environment},{"connected",true},
           {"port",result.at("port")},{"configuredProviders",result.at("configuredProviders")}});
    return 0;
}

volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }

void daemon(Arguments& args, const std::filesystem::path& home, Bots& bots) {
    bool fixed = false, web = true;
    std::optional<unsigned> port;
    std::optional<std::string> sessionRead;
    RuntimeOptions options;
    while (!args.empty()) {
        const auto option = args.pop("run option");
        if (option == "--fixed") fixed = true;
        else if (option == "--no-web") web = false;
        else if (option == "--port") port = static_cast<unsigned>(number(args,"port",0,65535));
        else if (option == "--session-read") {
            sessionRead = args.pop("session read mode");
            if (*sessionRead != "off" && *sessionRead != "on_demand" && *sessionRead != "prefill")
                throw Conflict("Session read mode must be off, on_demand or prefill");
        } else throw Conflict("Unknown run option: " + option);
    }
    Store store(home / "flock.sqlite3",[&](std::string_view id) { return bots.initialMemory(id); });
    SettingsSystem system(home,port,sessionRead);
    SettingsConnections connections(home);
    SettingsAccess access(store,home,std::getenv("FLOCK_WEB_IDENTITIES_FILE"));
    const auto identities = access.identities();
    options.sessionRead = system.sessionRead();
    const auto savedPort = T3Connections(store).port();
    if (savedPort && !web)
        std::cerr << "Warning: T3 skills cannot reach Flock in headless mode. Start 'flock run' with Web enabled.\n";
    if (const auto token = connections.githubToken(); !token.empty()) {
        options.gitHost = makeGitHubHost(token);
        store.protectGitCredential(token);
    }
    std::unique_ptr<Model> model;
    std::unique_ptr<T3> t3;
    std::function<Json()> modelStatus;
    std::function<bool(const Person&,bool)> modelLogin;
    std::function<bool(const Person&)> modelSwitch;
    std::function<std::optional<bool>()> modelLogout;
    struct SignIn {
        std::mutex mutex;
        bool running = false;
        unsigned attempts = 0;
        std::string error;
        std::jthread worker;
    } signIn;
    if (fixed) {
        model = makeDemoModel();
        t3 = makeDemoT3();
        modelStatus = [] { return Json{{"connected",true},{"demo",true}}; };
    } else {
        auto chatgpt = std::make_unique<ChatGPTModel>(home / "credentials" / "chatgpt");
        modelStatus = [model = chatgpt.get(),&signIn] {
            unsigned attempts;
            {
                std::lock_guard lock(signIn.mutex);
                if (signIn.running) return Json{{"signingIn",true}};
                attempts = signIn.attempts;
            }
            Json status;
            try { status = model->webStatus(); }
            catch (const std::exception&) { status = {{"unavailable",true}}; }
            std::lock_guard lock(signIn.mutex);
            if (signIn.running) return Json{{"signingIn",true}};
            if (signIn.attempts != attempts) status = {{"unavailable",true}};
            if (!signIn.error.empty()) status["signInError"] = signIn.error;
            return status;
        };
        const auto startSignIn = [&signIn,&access,directory = home / "credentials" / "chatgpt"](
            const Person& actor, bool reconsent, bool switchAccount) {
            std::lock_guard lock(signIn.mutex);
            if (signIn.running) return false;
            signIn.worker = std::jthread([&signIn,&access,directory,actor,reconsent,switchAccount](std::stop_token stop) {
                std::string error;
                try {
                    ChatGPTModel(directory).loginWeb(reconsent,stop,switchAccount,[&](const auto& commit) {
                        access.commitIfAdministrator(actor,commit);
                    });
                } catch (const DecisionDenied&) { error = "The account that started sign-in can no longer manage Flock."; }
                catch (const ModelError& failure) { error = failure.what(); }
                catch (const std::exception&) { error = "Could not sign in to ChatGPT."; }
                std::lock_guard lock(signIn.mutex);
                signIn.running = false;
                signIn.error = std::move(error);
            });
            signIn.running = true;
            ++signIn.attempts;
            signIn.error.clear();
            return true;
        };
        modelLogin = [startSignIn](const Person& actor, bool reconsent) { return startSignIn(actor,reconsent,false); };
        modelSwitch = [startSignIn](const Person& actor) { return startSignIn(actor,false,true); };
        modelLogout = [&signIn,account = chatgpt.get()]() -> std::optional<bool> {
            std::lock_guard lock(signIn.mutex);
            if (signIn.running) return std::nullopt;
            ++signIn.attempts;
            const auto revoked = account->signOut().at("revocationConfirmed").get<bool>();
            signIn.error.clear();
            return revoked;
        };
        model = std::move(chatgpt);
        t3 = makeT3Mcp(home / "credentials" / "t3");
    }
    options.botTools = connections.loadTools();
    options.peers = [&](std::string_view botId) {
        auto peers = bots.peers(botId);
        std::erase_if(peers,[&](const PeerBot& peer) { return store.policy().botDeleted(peer.intake.botId); });
        return peers;
    };
    options.automationError = [](std::string_view error) { std::cerr << "Unable to fire automations: " << error << '\n'; };
    options.storageContention = [](std::string_view error) { std::cerr << "Flock storage busy: " << error << '\n'; };
    const IntakeResolver intake = [&](std::string_view botId, std::string_view request) {
        return bots.get(botId).intake(request);
    };
    options.intake = intake;
    const Runtime::SoulLoader soul = [&](std::string_view botId) { return bots.soul(botId); };
    options.applyBotChange = [&](const BotChange& change) { bots.apply(change); };
    options.reconcileBotCreation = [&](const BotChange& change) { return bots.reconcileCreation(change); };
    Runtime runtime(store,*model,*t3,soul,options);
    bots.syncSchedules(store,options.clock(),[](std::string_view id,std::string_view error) {
        std::cerr << "Flock Bot " << id << ": " << error << '\n';
    });
    Json exportedBots;
    // Web callbacks and the filesystem refresh both update the same on-disk skills.
    std::mutex skillMutex;
    auto syncSkills = [&](bool providersChanged = false) {
        if (!T3Connections(store).configured()) return;
        std::lock_guard lock(skillMutex);
        if (providersChanged) exportedBots = nullptr;
        try {
            auto descriptions = activeBotCards(store.policy(),bots.skillCards());
            if (descriptions != exportedBots) {
                syncT3Skills(descriptions);
                exportedBots = std::move(descriptions);
            }
        } catch (const std::exception& error) {
            std::cerr << "Could not refresh T3 skills: " << error.what() << '\n';
        }
    };
    if (web) syncSkills();
    WebCallbacks callbacks{
        [&] { return activeBotCards(store.policy(),bots.cards()); },
        intake,
        [&](const Person& creator,const Json& profile,const std::optional<AvatarImage>& image) {
            auto created = store.policy().createOwnedBot(creator,[&](const auto& publishOwned) {
                return bots.create(profile,image,publishOwned);
            });
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
        options.clock,
    };
    callbacks.resolveIdentity = identities.resolveRoot;
    callbacks.configuredPerson = identities.resolvePerson;
    callbacks.identityRequired = [&access] { return access.enabled(); };
    connections.configureChannels(store,callbacks);
    callbacks.sessionT3 = t3.get();
    callbacks.modelStatus = modelStatus;
    callbacks.modelLogin = modelLogin;
    callbacks.modelSwitch = modelSwitch;
    callbacks.modelLogout = modelLogout;
    callbacks.installationSettings = [&](const Person& actor) {
        auto availableConnections = connections.view();
        availableConnections.at("channels")["canManageHost"] = actor == localOwner;
        return Json{{"access",access.view(actor)},{"system",system.view(T3Connections(store).port())},
                    {"connections",std::move(availableConnections)}};
    };
    callbacks.changeInstallationSettings = [&](const Person& actor, std::string_view section, const Json& args) {
        if (section == "access") return access.change(actor,args);
        if (section == "system") return system.change(args,T3Connections(store).port());
        if (section == "channels" && actor != localOwner) {
            const auto action = args.value("action","");
            if (action == "saveMachine" || action == "removeMachine" ||
                (action == "options" && args.contains("sessionCredential"))) throw DecisionDenied{};
        }
        return connections.change(section,args);
    };
    callbacks.botChannels = [&](std::string_view bot) { return connections.botChannels(bot); };
    callbacks.changeBotChannel = [&](std::string_view bot, const Json& args) {
        return connections.changeBotChannel(bot,args);
    };
    std::atomic<unsigned> webPort = 0;
    struct T3Setup {
        std::mutex mutex;
        bool running = false;
        std::string requester, authorizationUrl, error;
        std::jthread worker;
    } t3Setup;
    callbacks.t3Disconnect = [&](const std::optional<std::string>& environment) { disconnectT3(store,environment); };
    if (!fixed) {
        callbacks.t3Setup = [&t3Setup](const Person& actor) {
            std::lock_guard lock(t3Setup.mutex);
            const bool ownsAttempt = actor.id == t3Setup.requester;
            return Json{{"running",t3Setup.running},
                        {"authorizationUrl",ownsAttempt ? t3Setup.authorizationUrl : std::string{}},
                        {"error",ownsAttempt ? t3Setup.error : std::string{}}};
        };
        callbacks.t3Connect = [&](const Person& actor, const std::string& environment, const std::string& mcpUrl) {
            const auto expected = environment.empty() ? std::nullopt : std::optional{environment};
            if (expected) validateT3ConnectTarget(*expected,mcpUrl);
            else validateT3McpUrl(mcpUrl);
            const auto port = webPort.load();
            if (!port) return false;
            std::lock_guard lock(t3Setup.mutex);
            if (t3Setup.running) return false;
            t3Setup.worker = std::jthread([&,port,expected,mcpUrl,requester = actor](std::stop_token stop) {
                std::string error;
                try {
                    connectT3(home,expected,mcpUrl,port,[&](const std::string& url) {
                        std::lock_guard lock(t3Setup.mutex);
                        t3Setup.authorizationUrl = url;
                    },stop,requester);
                    syncSkills(true);
                } catch (const Conflict& failure) { error = failure.what(); }
                catch (const std::exception&) { error = "Could not complete the T3 connection."; }
                std::lock_guard lock(t3Setup.mutex);
                t3Setup.running = false;
                t3Setup.authorizationUrl.clear();
                t3Setup.error = std::move(error);
            });
            t3Setup.running = true;
            t3Setup.requester = actor.id;
            t3Setup.authorizationUrl.clear();
            t3Setup.error.clear();
            return true;
        };
    }
    std::unique_ptr<WebServer> server;
    if (web) {
        callbacks.githubWebhookSecret = connections.githubWebhookSecret();
        const auto firstPort = system.startupPort(savedPort);
        const auto onBound = [&](unsigned boundPort) {
            rebindT3Port(store,boundPort);
            system.listenerStarted(boundPort);
            webPort.store(boundPort);
        };
        try {
            server = startWeb(store,firstPort,callbacks,onBound);
        } catch (const std::exception&) {
            const auto recovery = system.recoveryPort(savedPort);
            if (!recovery || *recovery == firstPort) throw;
            std::cerr << "Could not apply the saved Web port. Starting Flock on its previous port.\n";
            server = startWeb(store,*recovery,std::move(callbacks),onBound);
        }
        std::cout << "Flock Web UI at http://127.0.0.1:" << server->port() << std::endl;
    }
    std::signal(SIGINT,requestStop);
    std::signal(SIGTERM,requestStop);
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

void rulesCommand(const std::filesystem::path& home, Arguments& args) {
    const auto command = args.pop("rules command");
    auto store = openStore(home);
    auto policy = store.policy();
    if (command == "list") {
        args.end();
    } else {
        if (command != "add" && command != "remove") throw Conflict("Unknown rules command");
        const auto kind = parseRuleKind(args.pop("rule kind"));
        if (!kind) throw Conflict("Unknown rule kind");
        const auto action = parseAction(args.pop("policy action"));
        if (!action) throw Conflict("Unknown policy action");
        const auto target = args.pop("target");
        args.end();
        policy.changeRule(localOwner,*kind,*action,target,command == "add");
    }
    print(policy.rules());
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
        auto cards = bots.cards();
        if (std::filesystem::exists(home / "flock.sqlite3")) cards = activeBotCards(openStore(home).policy(),std::move(cards));
        print(cards);
    } else if (command == "run") daemon(args,home,bots);
    else if (command == "rules") rulesCommand(home,args);
    else if (command == "mode") {
        auto store = openStore(home);
        if (!args.empty()) {
            const auto mode = args.pop("mode");
            args.end();
            store.policy().setMode(localOwner,mode);
        }
        std::cout << store.policy().mode() << '\n';
    }
    else if (command == "message") {
        const auto botId = args.pop("Bot ID");
        const auto message = args.pop("message");
        args.end();
        Intake intake;
        try { intake = bots.get(botId).intake(message); }
        catch (const std::exception&) {
            const auto known = bots.ids();
            if (std::ranges::find(known,botId) == known.end()) throw;
            // A known Bot can accept messages before its Session settings are complete.
            intake.botId = botId;
            intake.request = message;
        }
        auto store = openStore(home);
        print(store.receive(localOwner,intake,actionId(),message));
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
