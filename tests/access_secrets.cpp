#include "flock/policy.hpp"
#include <barrier>
#include <sqlite3.h>
#include <filesystem>
#include <future>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace flockv0;
namespace {
void check(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
template<class F> void denied(F action) {
    try { action(); } catch (const Conflict&) { return; }
    throw std::runtime_error("Expected denial");
}
constexpr auto credential = "sensitive-test-value";
struct ToolModel : Model {
    Decision next;
    Json result;
    ModelTurn decide(const ModelInput& input) override { return {{choose(input)}}; }
    Decision choose(const ModelInput& input) {
        for (auto& data:{input.task,input.event,input.history,input.evidence,input.config,Json(input.notes.soul),Json(input.feedback)})
            check(data.dump().find(credential)==std::string::npos,"value reached model");
        if (next.tool.empty()) {
            result=input.event;
            return {"reply",{{"text","acknowledged"}},""};
        }
        auto decision=next;
        next={};
        return decision;
    }
};
struct Adapter : T3 {
    bool connected = true;
    std::string authGeneration(const std::string&,const Json&) override {return connected ? "fixture" : "";}
    Delivery send(const Command&) override {throw Conflict("Unexpected send");}
    Delivery reconcile(const Command&) override {return {};}
    Observation observe(const std::string&,const Json&,const RunRef&) override {return {};}
    InterruptOutcome interrupt(const std::string&,const Json&,const RunRef&) override { return InterruptOutcome::Requested; }
};
const Person owner{"local-owner",true}, admin{"alice"}, viewer{"bob"};
const Resource readRepo{Action::repository_read,"github.com/org/private"};
struct WorkModel : Model {
    ModelTurn decide(const ModelInput& in) override { return {{choose(in)}}; }
    Decision choose(const ModelInput& in) {
        if (in.task.value("request","") == "ask reviewer")
            return {"send_bot_message",{{"kind","work"},{"botId","Reviewer"},{"text","review repository"},{"doneWhen","reviewed"}},"Request peer review"};
        return {"delegate",{{"instruction","read repository"},{"doneWhen","read complete"}},"Read the requested repository"};
    }
};
struct RunningAdapter : Adapter {
    std::map<std::string,RunRef> running;
    Delivery send(const Command& cmd) override {
        RunRef run{"thread-" + cmd.ownerBotId,"run-" + cmd.ownerBotId,{}};
        running[cmd.ownerBotId] = run;
        return {Delivery::Kind::Accepted,run,{}};
    }
};

void runningSessions(const std::filesystem::path& path) {
    Store store(path);
    auto access = store.policy();
    WorkModel model;
    RunningAdapter adapter;
    RuntimeOptions options;
    options.peers = [](std::string_view sender) -> std::vector<PeerBot> {
        if (sender == "Ernest") return {{"Reviewer",{"Reviewer","","env","peer-project"}}};
        return {};
    };
    Runtime runtime(store,model,adapter,[](auto){return std::string{};},options);
    store.submit({"Ernest","read repository","env","project"});
    store.submit({"Ernest","ask reviewer","env","other-project"});
    for(int i=0; i<30 && adapter.running.size()<2; ++i) runtime.tick();
    check(adapter.running.size()==2,"peer and sender did not launch under distinct owners");
    std::vector<std::string> completed;
    auto read = [&](const std::string& bot, bool upstream = true) {
        access.executeSessionResource("env",adapter.running.at(bot),readRepo,[&]{ return upstream; },[&](Id task){
            check(store.task(task).at("botId")==bot,"execution callback received the wrong owner");
            completed.push_back(bot);
        });
    };
    read("Ernest");
    read("Reviewer");
    denied([&]{read("Reviewer",false);});
    Store authority(path);
    denied([&] {
        access.executeSessionResource("env",adapter.running.at("Ernest"),readRepo,[&] {
            authority.policy().changeNeverRule(owner,readRepo.action,readRepo.target,true);
            return true;
        },[&](Id){ completed.push_back("denied"); });
    });
    denied([&]{read("Ernest");});
    denied([&]{read("Reviewer");});
    check(completed == std::vector<std::string>{"Ernest","Reviewer"},"Never rule did not apply across Bots");
}

void policyRules(const std::filesystem::path& path) {
    Store store(path);
    auto policy = store.policy();
    check(policy.mode() == "full" && policy.rules().empty(),"fresh installation is not Full");
    check(policy.check("Ernest",readRepo.action,readRepo.target) == PolicyResult::allow,"Full denied a read");
    policy.changeNeverRule(owner,readRepo.action,readRepo.target,true);
    policy.changeNeverRule(owner,Action::repository_write,"github.com/*/private*",true);
    check(policy.check("Reviewer",readRepo.action,readRepo.target) == PolicyResult::deny &&
          policy.allows("Ernest",readRepo.action,"github.com/org/public") &&
          !policy.allows("Ernest",Action::repository_write,"github.com/org/private-1") &&
          !policy.allows("Ernest",Action::repository_write,"github.com//private") &&
          policy.allows("Ernest",Action::repository_write,"github.com/org/public-private") &&
          policy.allows("Ernest",Action::repository_review,"github.com/org/private"),
          "exact or wildcard matching failed");
    policy.changeNeverRule(owner,readRepo.action,readRepo.target,false);
    Store reopened(path);
    check(reopened.policy().rules().size() == 1 && reopened.policy().allows("Ernest",readRepo),
          "rule changes were not persisted");
    denied([&] { policy.changeNeverRule(admin,readRepo.action,readRepo.target,true); });

    policy.setOwner(admin,"Ernest");
    policy.setMember(admin,"Ernest",viewer.id,true);
    const Person stranger{"stranger"};
    for (auto action : {Action::bot_view,Action::bot_talk,Action::bot_decide})
        check(policy.allows(admin,action,"Ernest") && policy.allows(viewer,action,"Ernest") &&
              !policy.allows(stranger,action,"Ernest"),"owner or member admission failed");
    check(policy.allows(admin,Action::bot_settings,"Ernest") && policy.allows(owner,Action::bot_settings,"Ernest") &&
          !policy.allows(viewer,Action::bot_settings,"Ernest"),"settings were not restricted to the owner");
    denied([&] { policy.setMember(viewer,"Ernest",stranger.id,true); });
    policy.setMember(admin,"Ernest",viewer.id,false);
    check(!policy.allows(viewer,Action::bot_talk,"Ernest"),"removed member retained access");
}

void freshLaunch(const std::filesystem::path& path) {
    Store store(path);
    RunningAdapter adapter;
    WorkModel model;
    Runtime runtime(store,model,adapter,[](auto){ return std::string{}; });
    auto task = store.submit({"Fresh","read repository","env","project"});
    store.policy().changeNeverRule(owner,Action::environment_launch,"env",true);
    for (int i=0;i<6;++i) runtime.tick();
    check(adapter.running.empty() && store.task(task).at("wait").at("reason")=="settings",
          "pending launch permission reached T3");
}

void freshPendingSecretRequests(const std::filesystem::path& path) {
    check(!std::filesystem::exists(path),"secret schema fixture must start with a fresh database");
    Store left(path), right(path);
    auto access = left.policy();
    check(access.administration(owner,"Ernest").at("secretInputs").empty(),"fresh protected input table is not empty");
    const SecretRef ref{"fixture-token"};
    std::barrier start(2);
    auto request = [&](Store& store) {
        start.arrive_and_wait();
        return store.policy().requestSecret("Ernest",ref);
    };
    auto other = std::async(std::launch::async,request,std::ref(right));
    const auto id = request(left);
    check(id == other.get(),"repeated secret request returned a different ID");
    check(right.policy().requestSecret("Reviewer",ref) != id,"pending secret uniqueness crossed Bot boundaries");
    denied([&]{ access.submitSecret(owner,"Ernest",id,"nest"); });
    access.submitSecret(owner,"Ernest",id,credential);
    check(access.administration(owner,"Ernest").at("secretInputs") ==
          Json::array({{{"id",id},{"name",ref.name},{"status","fulfilled"}}}),"duplicate request remained after fulfillment");
    denied([&]{ access.requestSecret("Ernest",ref); });
    denied([&]{ access.requestSecret("Reviewer",ref); });
}

void creationRecoversWithOriginalOwner(const std::filesystem::path& path) {
    const Person creator{"creator",false};
    Store store(path);
    store.policy().setOwner(creator,"Ernest");
    ToolModel model;
    Adapter t3;
    RuntimeOptions options;
    std::string appliedKey;
    options.applyBotChange = [&](const BotChange& change) {
        sqlite3* connection = nullptr;
        const auto opened = sqlite3_open_v2(path.c_str(),&connection,SQLITE_OPEN_READONLY,nullptr);
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> checkDb(connection,sqlite3_close);
        check(opened == SQLITE_OK,"Could not open a separate connection for creation recovery");
        sqlite3_stmt* statement = nullptr;
        check(sqlite3_prepare_v2(connection,"SELECT request_key,person FROM bot_creation_intents",-1,&statement,nullptr)
              == SQLITE_OK,"Could not read committed creation intent");
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> row(statement,sqlite3_finalize);
        check(sqlite3_step(statement) == SQLITE_ROW &&
              std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(statement,0))) == change.requestKey &&
              std::string_view(reinterpret_cast<const char*>(sqlite3_column_text(statement,1))) == creator.id,
              "Bot creation ran before its original intent was durable");
        if (appliedKey.empty()) {
            appliedKey = change.requestKey;
            throw std::runtime_error("Interrupted after profile publication");
        }
        check(appliedKey == change.requestKey,"Recovery used a different publication key");
    };
    const auto run = [&](Store& db) {
        Runtime runtime(db,model,t3,[](std::string_view){return std::string{};},options);
        runtime.runConversations();
    };
    model.next = {"request_bot_change",{{"operation","create_bot"},{"targetBotId","NewBot"},{"name","New Bot"}},{}};
    store.receive(creator,Intake{"Ernest","","env","project"},"create","Create a Bot");
    try { run(store); }
    catch (const std::runtime_error& error) {
        check(std::string_view(error.what()) == "Interrupted after profile publication","Unexpected creation failure");
    }
    check(!appliedKey.empty() && !store.policy().allows(creator,Action::bot_settings,"NewBot"),
          "Interrupted creation finalized ownership prematurely");
    Store restarted(path);
    run(restarted);
    check(restarted.policy().allows(creator,Action::bot_settings,"NewBot") &&
          model.result.at("turn").back().at("result").at("status") == "applied","Creation did not recover with the original owner");
}

void exercise(const std::filesystem::path& dir) {
    auto path=dir/"state.db";
    Store store(path);
    auto access=store.policy();
    Intake ernest{"Ernest","read","env","project"}, reviewer{"Reviewer","review","env","project"};
    auto child=store.submit(ernest), peerChild=store.submit(reviewer);
    ToolModel model;
    Adapter t3;
    std::vector<BotChange> applied;
    RuntimeOptions options;
    options.applyBotChange = [&](const BotChange& change) { applied.push_back(change); };
    Runtime runtime(store,model,t3,[](std::string_view){return std::string{};},options);
    unsigned message=0;
    auto turn=[&](std::string name, Json args=Json::object(),std::string text="acknowledged") {
        model.next={std::move(name),std::move(args),""};
        store.receive(localOwner,ernest,std::to_string(++message),text);
        runtime.runConversations();
        return model.result.dump();
    };
    for (const auto* removed : {"list_grants","request_access"})
        check(turn(removed).find("Tool unavailable")!=std::string::npos,"removed tool was offered");
    ernest.config["services"]={"github"};
    ernest.config["mcpServers"]={"docs"};
    auto connections=turn("list_connections");
    check(connections.find("github")!=std::string::npos && connections.find("docs")!=std::string::npos,"connection metadata");
    check(turn("request_secret_input",{{"name","repo-token"}}).find("pending")!=std::string::npos,"protected input request");
    auto metadata=access.administration(owner,"Ernest");
    Id input=metadata.at("secretInputs").at(0).at("id");
    check(turn("submit_secret",{{"requestId",input},{"value","text-supplied-value"}}).find("Tool unavailable") !=
              std::string::npos && store.decisionHistory(owner).empty(),
          "A Bot response fulfilled a protected decision");
    denied([&]{access.submitSecret(viewer,"Ernest",input,credential);});
    access.submitSecret(owner,"Ernest",input,credential);
    denied([&]{ access.requestSecret("Ernest",{"../outside"}); });
    denied([&]{ access.submitSecret(admin,"Ernest",access.requestSecret("Ernest",{"other"}),"other-value"); });
    const Json sensitiveFields{{"kind",credential},{"status",credential},{"secret",credential}};
    check(access.redactSecrets("Ernest",sensitiveFields).dump().find(credential)==std::string::npos,
          "external field mistaken for a trusted protocol tag");
    const auto vaultFile = std::filesystem::directory_iterator(dir/"secrets")->path();
    struct stat st{};
    check(!stat((dir/"secrets").c_str(),&st) && (st.st_mode&0777)==0700,"directory permissions");
    check(!stat(vaultFile.c_str(),&st) && (st.st_mode&0777)==0600,"secret permissions");
    check(turn("list_secrets",Json::object(),credential).find("repo-token")!=std::string::npos,"names unavailable");
    access.useSecret(child,{"repo-token"},true,[](auto v){check(v==credential,"resolution");});
    access.useSecret(peerChild,{"repo-token"},true,[](auto v){check(v==credential,"shared resolution");});
    access.changeNeverRule(owner,Action::secret_use,"repo-*",true);
    denied([&]{access.useSecret(peerChild,{"repo-token"},true,[](auto){});});
    access.changeNeverRule(owner,Action::secret_use,"repo-*",false);
    try {
        access.useSecret(child,{"repo-token"},true,[](auto v){throw Conflict(std::string(v));});
        throw std::runtime_error("adapter exception disappeared");
    }
    catch(const Conflict& e){check(std::string(e.what()).find(credential)==std::string::npos,"adapter error leaked");}
    access.changeNeverRule(owner,Action::environment_launch,"env",true);
    check(turn("check_environment_readiness",{{"environmentId","env"}}).find("/settings/Ernest")!=std::string::npos,"missing setup link");
    access.changeNeverRule(owner,Action::environment_launch,"env",false);
    check(turn("check_environment_readiness",{{"environmentId","env"}}).find("\"ready\":true")!=std::string::npos,"readiness");
    t3.connected = false;
    check(turn("check_environment_readiness").find("T3 sign-in")!=std::string::npos,"OAuth denial");
    t3.connected = true;
    access.changeNeverRule(owner,Action::environment_configure,"env",true);
    check(turn("update_bot_settings",{{"environmentId","env"},{"runTimeoutSeconds",123}}).find("configure access required")!=std::string::npos,
          "settings mutation accepted without a grant");
    access.changeNeverRule(owner,Action::environment_configure,"env",false);
    turn("update_bot_settings",{{"environmentId","env"},{"runTimeoutSeconds",123}});
    check(turn("list_connections").find("\"runTimeoutSeconds\":123")!=std::string::npos,"settings not effective");
    for (auto op : {"create_bot","rename_bot"})
        check(turn("request_bot_change",{{"operation",op},{"targetBotId","Target"},{"name","New"}})
                  .find("\"status\":\"applied\"") != std::string::npos,"Full did not apply a Bot change");
    check(applied.size() == 2 && store.inbox(owner).size() == 1,"Full queued a Bot change for review");
    denied([&]{access.applyBotChange("Ernest",Person{"member",false},"rename_bot","Target","Hijacked",0,0,
                                      options.applyBotChange);});
    check(applied.size() == 2,"a non-owner renamed another person's Bot");
    access.changeNeverRule(owner,Action::bot_rename,"Target",true);
    check(turn("request_bot_change",{{"operation","rename_bot"},{"targetBotId","Target"},{"name","Denied"}})
              .find("denied by policy") != std::string::npos && applied.size() == 2,"Never allowed a Bot change");
    check(store.conversation("Ernest").dump().find(credential)==std::string::npos &&
          access.administration(owner,"Ernest").dump().find(credential)==std::string::npos,"value in conversation or audit");
    check(chmod(vaultFile.c_str(),0644)==0,"fixture mode");
    denied([&]{access.useSecret(child,{"repo-token"},true,[](auto){});});
    check(chmod(vaultFile.c_str(),0600)==0,"restore fixture mode");
    auto linkTarget = dir/"outside";
    std::filesystem::rename(vaultFile,linkTarget);
    std::filesystem::create_symlink(linkTarget,vaultFile);
    denied([&]{access.useSecret(child,{"repo-token"},true,[](auto){});});
    std::filesystem::remove(vaultFile);
    std::filesystem::rename(linkTarget,vaultFile);
    std::filesystem::rename(dir/"secrets",dir/"original-vault");
    std::filesystem::create_directory_symlink(dir/"original-vault",dir/"secrets");
    denied([&]{access.useSecret(child,{"repo-token"},true,[](auto){});});
    std::filesystem::remove(dir/"secrets");
    std::filesystem::rename(dir/"original-vault",dir/"secrets");
    model.next={"list_secrets",Json::object(),"list"};
    store.receive(localOwner,reviewer,"peer-names","show names",0);
    runtime.runConversations();
    check(model.result.at("turn").at(0).at("result").at("names") == Json::array({"repo-token"}),"shared secret names unavailable");
    check(access.redactSecrets("Reviewer",Json(credential))=="[redacted]" &&
          access.redactSecrets("Ernest",Json(credential))=="[redacted]","redaction crossed Bot authority");
    for (auto suffix:{"","-wal"}) {
        std::ifstream file(path.string()+suffix,std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)),{});
        check(content.find(credential)==std::string::npos,"value stored in SQLite");
    }
}
}
int main() {
    char tmp[]="/tmp/flock-access-XXXXXX";
    if (!mkdtemp(tmp)) return 1;
    try {
        const std::filesystem::path directory(tmp);
        policyRules(directory/"race.db");
        freshLaunch(directory/"fresh.db");
        runningSessions(directory/"peers.db");
        freshPendingSecretRequests(directory/"secret-requests"/"state.db");
        creationRecoversWithOriginalOwner(directory/"creation-recovery.db");
        exercise(directory);
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);
}
