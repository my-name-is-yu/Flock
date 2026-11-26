#include "bots.hpp"
#include "flock/decision_errors.hpp"
#include "demo.hpp"
#include "chatgpt_web.hpp"
#include "model.hpp"
#include "git_github.hpp"
#include "settings_access.hpp"
#include "settings_connections.hpp"
#include "settings_system.hpp"
#include "t3_connection_setup.hpp"
#include "t3_setup.hpp"
#include "flock/tasks.hpp"
#include "flock/git_events.hpp"
#include "flock/conversations.hpp"
#include "flock/bot_change.hpp"
#include "flock/bot_profile.hpp"
#include "flock/external_mcp.hpp"
#include "flock/identity.hpp"
#include "flock/intake.hpp"
#include "flock/json.hpp"
#include "flock/model.hpp"
#include "flock/model_error.hpp"
#include "flock/person.hpp"
#include "flock/policy.hpp"
#include "flock/runtime.hpp"
#include "flock/store.hpp"
#include "flock/t3.hpp"
#include "flock/t3_connection.hpp"
#include "flock/t3_mcp.hpp"
#include "flock/task.hpp"
#include "flock/types.hpp"
#include "flock/wait.hpp"
#include "flock/web.hpp"
#include "flock/task_target.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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
  t3 connect [MCP_URL] [--port PORT]
  t3 reconnect ENVIRONMENT [--port PORT]
  t3 disconnect ENVIRONMENT
  external-mcp connect PERSON BOT [BOT ...]
  external-mcp disconnect CONNECTION_ID
  model login [--print-url] [--reconsent]
  model status
  model models
  model acceptance MODEL_SLUG

Approve continues a paused request for another full allowance.
State defaults to $FLOCK_HOME or ~/.config/flock. Quote messages and answers
containing spaces. Run --fixed uses local demo adapters for both model and T3.
)";

class Arguments {
public:
    Arguments(int argc, char** argv) : remaining_(argc - 1), next_(argv + 1) {}
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
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size() || value < lower || value > upper)
        throw Conflict("Invalid " + std::string(name));
    return value;
}

Id number(Arguments& args, std::string_view name, Id lower = 1, Id upper = INT64_MAX) {
    return integer(args.pop(name), name, lower, upper);
}

std::filesystem::path homePath(Arguments& args) {
    if (args.peek() == "--home") {
        args.pop("--home");
        const auto path = args.pop("Flock home");
        if (path.empty()) throw Conflict("Flock home must not be empty");
        return std::filesystem::absolute(path);
    }
    if (const auto* configured = std::getenv("FLOCK_HOME"); configured && *configured) return std::filesystem::absolute(configured);
    if (const auto* home = std::getenv("HOME"); home && *home) return std::filesystem::path(home) / ".config" / "flock";
    throw Conflict("Set FLOCK_HOME or provide --home DIRECTORY");
}

Store openStore(const std::filesystem::path& home) {
    return Store(home / "flock.sqlite3");
}

Json activeBotCards(const Store& store, Json cards) {
    cards.erase(std::remove_if(cards.begin(), cards.end(),
                               [&](const Json& card) { return store.botDeleted(card.at("id").get_ref<const std::string&>()); }),
                cards.end());
    return cards;
}

void print(const Json& value) {
    std::cout << value.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
}

int modelCommand(Arguments& args, const std::filesystem::path& home) {
    ChatGPTModel model(home / "credentials" / "chatgpt");
    const auto action = args.pop("model command");
    if (action == "login") {
        bool printUrl = false, reconsent = false;
        while (!args.empty()) {
            const auto option = args.pop("login option");
            if (option == "--print-url")
                printUrl = true;
            else if (option == "--reconsent")
                reconsent = true;
            else
                throw Conflict("Unknown model login option: " + option);
        }
        const auto result = model.login(printUrl, reconsent);
        print(result);
        if (!result.value("planUsage", false)) {
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
        return runModelAcceptance(model, slug, std::cout) ? 0 : 1;
    } else
        throw Conflict("Unknown model command: " + action);
    return 0;
}

int t3Command(Arguments& args, const std::filesystem::path& home, Bots& bots) {
    const auto action = args.pop("T3 command");
    if (action != "connect" && action != "reconnect" && action != "disconnect") throw Conflict("Unknown T3 command");
    if (action == "disconnect") {
        const auto environment = args.pop("canonical environment ID");
        args.end();
        auto store = openStore(home);
        disconnectT3(store, environment);
        print({{"environmentId", environment}, {"connected", false}});
        return 0;
    }
    using Source = T3ConnectRequest::Source;
    T3ConnectRequest request{Source::Local, {}};
    if (action == "reconnect")
        request = {Source::Saved, args.pop("canonical environment ID")};
    else if (!args.peek().empty() && !args.peek().starts_with("--")) {
        request = {Source::Url, args.pop("T3 connection URL")};
        validateT3McpUrl(request.value);
    }
    unsigned port = 0;
    while (!args.empty()) {
        const auto flag = args.pop("T3 connection option");
        if (flag == "--port")
            port = static_cast<unsigned>(number(args, "port", 1, 65535));
        else
            throw Conflict("Unknown T3 connection option: " + flag);
    }
    const auto result = connectT3(home, request, port, [](const std::string& authorization) {
        std::cerr << "Open this URL in your browser to authorize Flock's T3 connection.\n" << authorization << '\n';
    });
    if (result.portChanged)
        std::cerr << "T3 skills use Flock Web port " << result.port << ". Restart 'flock run' if it is already running.\n";
    try {
        syncT3Skills(activeBotCards(openStore(home), bots.skillCards()));
    } catch (const std::exception& error) {
        std::cerr << "Could not export T3 skills: " << error.what() << '\n';
    }
    print({{"environmentId", result.environmentId},
           {"connected", true},
           {"port", result.port},
           {"configuredProviders", result.configuredProviders}});
    return 0;
}

int externalMcpCommand(Arguments& args, const std::filesystem::path& home, const Bots& bots) {
    const auto action = args.pop("external MCP command");
    if (action == "connect") {
        const auto person = args.pop("Flock person");
        std::vector<std::string> allowedBots;
        do {
            const auto bot = args.pop("Bot ID");
            bots.get(bot);
            allowedBots.emplace_back(bot);
        } while (!args.empty());
        auto store = openStore(home);
        const SettingsAccess access(store, home, std::getenv("FLOCK_WEB_IDENTITIES_FILE"));
        const auto credential = ExternalMcp(store).connect(person, allowedBots);
        print({{"connectionId", credential.connectionId}, {"bearerToken", credential.bearerToken}});
    } else if (action == "disconnect") {
        const auto id = args.pop("external MCP connection ID");
        args.end();
        auto store = openStore(home);
        ExternalMcp(store).disconnect(id);
        print({{"connectionId", id}, {"connected", false}});
    } else
        throw Conflict("Unknown external MCP command");
    return 0;
}

volatile std::sig_atomic_t stopRequested = 0;
void requestStop(int) {
    stopRequested = 1;
}

void daemon(Arguments& args, const std::filesystem::path& home, Bots& bots) {
    bool fixed = false, web = true;
    std::optional<unsigned> port;
    std::optional<std::string> sessionRead;
    RuntimeOptions options;
    while (!args.empty()) {
        const auto option = args.pop("run option");
        if (option == "--fixed")
            fixed = true;
        else if (option == "--no-web")
            web = false;
        else if (option == "--port")
            port = static_cast<unsigned>(number(args, "port", 0, 65535));
        else if (option == "--session-read") {
            sessionRead = args.pop("session read mode");
            if (*sessionRead != "off" && *sessionRead != "on_demand" && *sessionRead != "prefill")
                throw Conflict("Session read mode must be off, on_demand or prefill");
        } else
            throw Conflict("Unknown run option: " + option);
    }
    const IntakeResolver intake = [&](std::string_view botId, std::string_view request) { return bots.get(botId).intake(request); };
    Store store(
        home / "flock.sqlite3", [&](std::string_view id) { return bots.initialMemory(id); },
        [&](std::string_view id) { return bots.plannerModel(id); });
    SettingsSystem system(home, port, sessionRead);
    SettingsConnections connections(home);
    SettingsAccess access(store, home, std::getenv("FLOCK_WEB_IDENTITIES_FILE"));
    const auto identities = access.identities();
    options.sessionRead = system.sessionRead();
    const auto savedPort = T3Connections(store).port();
    if (savedPort && !web) std::cerr << "Warning: T3 skills cannot reach Flock in headless mode. Start 'flock run' with Web enabled.\n";
    if (const auto token = connections.githubToken(); !token.empty()) {
        options.gitHost = makeGitHubHost(token);
        GitEvents(store).protectGitCredential(token);
    }
    std::unique_ptr<Model> model;
    std::unique_ptr<T3> t3;
    std::optional<ChatGPTWeb> chatgptWeb;
    if (fixed) {
        model = makeDemoModel();
        t3 = makeDemoT3();
    } else {
        auto chatgpt = std::make_unique<ChatGPTModel>(home / "credentials" / "chatgpt");
        chatgptWeb.emplace(*chatgpt, access);
        model = std::move(chatgpt);
        t3 = makeT3Mcp(home / "credentials" / "t3");
    }
    options.requireT3Connection = !fixed;
    options.botTools = connections.loadTools();
    options.peers = [&](std::string_view botId) {
        auto peers = bots.peers(botId);
        std::erase_if(peers, [&](const PeerBot& peer) { return store.botDeleted(peer.intake.botId); });
        return peers;
    };
    options.automationError = [](std::string_view error) { std::cerr << "Unable to fire automations: " << error << '\n'; };
    options.storageContention = [](std::string_view error) { std::cerr << "Flock storage busy: " << error << '\n'; };
    options.intake = intake;
    const Runtime::SoulLoader soul = [&](std::string_view botId) { return bots.soul(botId); };
    options.applyBotChange = [&](const BotChange& change) { bots.apply(change); };
    options.reconcileBotCreation = [&](const BotChange& change) { return bots.reconcileCreation(change); };
    Runtime runtime(store, *model, *t3, soul, options);
    const auto reportSchedule = [](std::string_view id, std::string_view error) {
        std::cerr << "Flock Bot " << id << ": " << error << '\n';
    };
    bots.syncSchedules(store, options.clock(), reportSchedule);
    Json exportedBots;
    // Web callbacks and the filesystem refresh both update the same on-disk skills.
    std::mutex skillMutex;
    auto syncSkills = [&](bool providersChanged = false) {
        if (!T3Connections(store).configured()) return;
        std::lock_guard lock(skillMutex);
        if (providersChanged) exportedBots = nullptr;
        try {
            auto descriptions = activeBotCards(store, bots.skillCards());
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
        [&] { return activeBotCards(store, bots.cards()); },
        intake,
        [&](const Person& creator, const Json& profile, const std::optional<AvatarImage>& image) {
            auto created =
                Policy{store}.createOwnedBot(creator, [&](const auto& publishOwned) { return bots.create(profile, image, publishOwned); });
            syncSkills();
            return created;
        },
        {},
        [&](std::string_view botId) { return bots.avatarImage(botId); },
        options.peers,
        identities.authenticate,
        options.clock,
    };
    callbacks.resolveIdentity = identities.resolveRoot;
    callbacks.configuredPerson = identities.resolvePerson;
    callbacks.identityRequired = [&access] { return access.enabled(); };
    connections.configureChannels(store, callbacks);
    callbacks.sessionT3 = t3.get();
    if (chatgptWeb)
        chatgptWeb->configure(callbacks);
    else {
        callbacks.modelStatus = [] { return Json{{"connected", true}, {"demo", true}}; };
        callbacks.modelCatalog = [] { return Json::array({{{"slug", "demo"}, {"displayName", "Demo"}}}); };
    }
    callbacks.updateBot = [&, modelCatalog = callbacks.modelCatalog](std::string_view botId, const Json& profile,
                                                                     const std::optional<AvatarImage>& image) {
        auto catalog = Json::array();
        if (!profile.value("reasoningEffort", "").empty()) {
            try {
                catalog = modelCatalog();
            } catch (const ModelError&) {
            }
        }
        auto updated = botRunSettingCount(profile) ? bots.updateRunSettings(store, botId, profile, options.clock(), reportSchedule, catalog)
                                                   : bots.updateProfile(botId, profile, image);
        syncSkills();
        return updated;
    };
    callbacks.installationSettings = [&](const Person& actor) {
        auto availableConnections = connections.view();
        availableConnections.at("channels")["canManageHost"] = actor == localOwner;
        return Json{{"access", access.view(actor)},
                    {"system", system.view(T3Connections(store).port())},
                    {"connections", std::move(availableConnections)}};
    };
    callbacks.changeInstallationSettings = [&](const Person& actor, std::string_view section, const Json& args) {
        if (section == "access") return access.change(actor, args);
        if (section == "system") return system.change(args, T3Connections(store).port());
        if (section == "channels" && actor != localOwner) {
            const auto action = args.value("action", "");
            if (action == "saveMachine" || action == "removeMachine" || (action == "options" && args.contains("sessionCredential")))
                throw DecisionDenied{};
        }
        return connections.change(section, args);
    };
    callbacks.botChannels = [&](std::string_view bot) { return connections.botChannels(bot); };
    callbacks.changeBotChannel = [&](std::string_view bot, const Json& args) { return connections.changeBotChannel(bot, args, store); };
    T3WebSetup t3Setup(home, [&] { syncSkills(true); });
    t3Setup.configure(callbacks, store, !fixed);
    std::unique_ptr<WebServer> server;
    if (web) {
        callbacks.githubWebhookSecret = connections.githubWebhookSecret();
        const auto firstPort = system.startupPort(savedPort);
        const auto onBound = [&](unsigned boundPort) {
            rebindT3Port(store, boundPort);
            system.listenerStarted(boundPort);
            t3Setup.listeningOn(boundPort);
        };
        try {
            server = startWeb(store, firstPort, callbacks, onBound);
        } catch (const std::exception&) {
            const auto recovery = system.recoveryPort(savedPort);
            if (!recovery || *recovery == firstPort) throw;
            std::cerr << "Could not apply the saved Web port. Starting Flock on its previous port.\n";
            server = startWeb(store, *recovery, std::move(callbacks), onBound);
        }
        std::cout << "Flock Web UI at http://127.0.0.1:" << server->port() << std::endl;
    }
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
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
    const auto task = number(args, "task ID");
    auto store = openStore(home);
    const auto status = Tasks(store).task(task);
    auto target = TaskTarget::from(status.task, 0);
    if (operation != "cancel" || !args.empty()) {
        target.waitId = number(args, "wait ID");
        target.revision = number(args, "revision ID", 0);
        target.generation = number(args, "generation", 0);
    }
    Json details = Json::object();
    if (operation == "answer")
        details["text"] = args.pop("answer");
    else if (operation == "extend")
        details["deadline"] = number(args, "new Unix deadline");
    else if (operation == "reconcile") {
        while (!args.empty()) {
            const auto flag = args.pop("reconciliation option");
            if (flag == "--thread")
                details["threadId"] = args.pop("Thread ID");
            else if (flag == "--run")
                details["runId"] = args.pop("Run ID");
            else if (flag == "--question")
                details["questionId"] = args.pop("question ID");
            else
                throw Conflict("Unknown reconciliation option: " + flag);
        }
    }
    args.end();

    if (operation == "reconcile") {
        details =
            Tasks(store).reconciliationData(task, target.waitId, std::move(details),
                                            [&](std::string_view id, std::string_view request) { return bots.get(id).intake(request); });
    }
    Tasks(store).operate(localOwner, target, operation, details);
    print(taskJson(Tasks(store).task(task)));
}

void taskCommand(const std::filesystem::path& home, const std::string& command, Arguments& args) {
    Id task = 0, cursor = 0;
    unsigned limit = 50;
    std::string bot;
    if (command == "history")
        task = number(args, "task ID");
    else if (!args.empty() && !args.peek().starts_with("--"))
        bot = args.pop("Bot ID");
    while (!args.empty()) {
        const auto flag = args.pop("pagination option");
        if ((command == "history" && flag == "--after") || (command == "tasks" && flag == "--before"))
            cursor = number(args, "cursor", 0);
        else if (flag == "--limit")
            limit = static_cast<unsigned>(number(args, "limit", 1, 200));
        else
            throw Conflict("Unknown pagination option: " + flag);
    }
    auto store = openStore(home);
    if (command == "history") {
        if (!Tasks(store).hasTask(task)) throw Conflict("Task does not exist");
        print(Tasks(store).history(task, cursor, limit));
    } else
        print(taskJson(Tasks(store).tasks(bot, cursor, limit)));
}

void rulesCommand(const std::filesystem::path& home, Arguments& args) {
    const auto command = args.pop("rules command");
    auto store = openStore(home);
    Policy policy{store};
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
        policy.changeRule(localOwner, *kind, *action, target, command == "add");
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
    if (command == "model") return modelCommand(args, home);
    Bots bots(home / "bots");
    if (command == "t3") return t3Command(args, home, bots);
    if (command == "external-mcp") return externalMcpCommand(args, home, bots);
    if (command == "bots") {
        args.end();
        auto cards = bots.cards();
        if (std::filesystem::exists(home / "flock.sqlite3")) cards = activeBotCards(openStore(home), std::move(cards));
        print(cards);
    } else if (command == "run")
        daemon(args, home, bots);
    else if (command == "rules")
        rulesCommand(home, args);
    else if (command == "mode") {
        auto store = openStore(home);
        if (!args.empty()) {
            const auto mode = args.pop("mode");
            args.end();
            Policy{store}.setMode(localOwner, mode);
        }
        std::cout << Policy{store}.mode() << '\n';
    } else if (command == "message") {
        const auto botId = args.pop("Bot ID");
        const auto message = args.pop("message");
        args.end();
        Intake intake;
        try {
            intake = bots.get(botId).intake(message);
        } catch (const std::exception&) {
            const auto known = bots.ids();
            if (std::ranges::find(known, botId) == known.end()) throw;
            // A known Bot can accept messages before its Session settings are complete.
            intake.botId = botId;
            intake.request = message;
        }
        auto store = openStore(home);
        print(Conversations(store).receive(localOwner, intake, actionId(), message));
    } else if (command == "status") {
        const auto id = number(args, "task ID");
        args.end();
        auto store = openStore(home);
        print(taskJson(Tasks(store).task(id)));
    } else if (command == "history" || command == "tasks")
        taskCommand(home, command, args);
    else if (command == "answer" || command == "approve" || command == "retry" || command == "extend" || command == "reconcile" ||
             command == "cancel")
        operate(home, bots, command, args);
    else
        throw Conflict("Unknown command: " + command + ". Run 'flock --help'.");
    return 0;
}
}
}

int main(int argc, char** argv) {
    // ChatGPT's in-process TLS transport must not write session keys to disk.
    unsetenv("SSLKEYLOGFILE");
    try {
        flockv0::app::Arguments args(argc, argv);
        return flockv0::app::runCli(args);
    } catch (const std::exception& error) {
        std::cerr << "flock: " << error.what() << '\n';
        return 1;
    }
}
