#include "flock/access.hpp"
#include <barrier>
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
    Decision decide(const ModelInput& input) override {
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
    void interrupt(const std::string&,const Json&,const RunRef&) override {}
};
const Person owner{"local-owner",true}, admin{"alice"}, viewer{"bob"};
const Scope readRepo{"repository","private","read"}, writeRepo{"repository","private","write"};
struct WorkModel : Model {
    Decision decide(const ModelInput& in) override {
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
    auto access = store.access();
    access.resourceGrant(owner,"Ernest",readRepo,GrantChange::grant);
    for (auto bot : {"Ernest","Reviewer"}) access.resourceGrant(owner,bot,{"environment","env","launch"},GrantChange::grant);
    WorkModel model;
    RunningAdapter adapter;
    RuntimeOptions options;
    options.peers = [](std::string_view sender) -> std::vector<PeerBot> {
        if (sender == "Ernest") return {{"Reviewer",{"Reviewer","","env","peer-project"}}};
        return {};
    };
    Runtime runtime(store,model,adapter,[](auto){return BotNotes{};},options);
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
    const auto ernestHistory = Scope::sessionRead("env",adapter.running.at("Ernest").threadId);
    check(access.permits("Ernest",ernestHistory,true) && !access.permits("Reviewer",ernestHistory,true) &&
          !access.permits("Ernest",ernestHistory,false),"Session ownership or OAuth scope incorrect");
    access.resourceGrant(owner,"Reviewer",ernestHistory,GrantChange::grant);
    check(access.permits("Reviewer",ernestHistory,true) &&
          !access.permits("Reviewer",Scope::sessionRead("other",adapter.running.at("Ernest").threadId),true),
          "session grant crossed environments");
    access.resourceGrant(owner,"Reviewer",ernestHistory,GrantChange::revoke);
    check(!access.permits("Reviewer",ernestHistory,true),"revoked shared Session stayed visible");
    read("Ernest");
    denied([&]{read("Reviewer");});
    access.resourceGrant(owner,"Reviewer",readRepo,GrantChange::grant);
    read("Reviewer");
    denied([&]{read("Reviewer",false);});
    Store authority(path);
    denied([&]{
        access.executeSessionResource("env",adapter.running.at("Ernest"),readRepo,[&]{
            authority.access().resourceGrant(owner,"Ernest",readRepo,GrantChange::revoke);
            return true;
        },[&](Id){ completed.push_back("revoked"); });
    });
    denied([&]{read("Ernest");});
    read("Reviewer");
    check(completed == std::vector<std::string>{"Ernest","Reviewer","Reviewer"},"completed actions after revocation");
}

void concurrentApproval(const std::filesystem::path& path) {
    Store left(path), right(path);
    auto access = left.access();
    auto id = access.request("Ernest",readRepo);
    denied([&]{ access.decide(owner,"Reviewer",id,true); });
    std::barrier start(2);
    auto decide = [&](Store& db) {
        start.arrive_and_wait();
        try { db.access().decide(owner,"Ernest",id,true); return true; }
        catch (const Conflict&) { return false; }
    };
    auto other = std::async(std::launch::async,decide,std::ref(right));
    const bool approved = decide(left);
    check(approved!=other.get(),"concurrent approval must apply exactly once");
    auto state = access.administration(owner,"Ernest");
    auto request = state.at("requests").at(0);
    check(state.at("grants").size()==1 && state.at("audit").size()==2 &&
          request.at("requestedBy")=="Ernest" && request.at("decidedBy")=="local-owner" &&
          request.at("requestedAt")>0 && request.at("decidedAt")>=request.at("requestedAt"),
          "grant request and decision audit diverged");
    access.resourceGrant(owner,"Ernest",readRepo,GrantChange::revoke);
    auto revocation = right.access().administration(owner,"Ernest").at("audit").back();
    check(revocation.at("action")=="revoke" && revocation.at("actor")==owner.id &&
          revocation.at("scope").at("resourceId")=="private","revocation audit missing");
}

void freshLaunch(const std::filesystem::path& path) {
    Store store(path);
    RunningAdapter adapter;
    WorkModel model;
    Runtime runtime(store,model,adapter,[](auto){ return BotNotes{}; });
    auto task = store.submit({"Fresh","read repository","env","project"});
    store.access().request("Fresh",{"environment","env","launch"});
    for (int i=0;i<6;++i) runtime.tick();
    check(adapter.running.empty() && store.task(task).at("wait").at("reason")=="settings",
          "pending launch permission reached T3");
}

void freshPendingSecretRequests(const std::filesystem::path& path) {
    check(!std::filesystem::exists(path),"secret schema fixture must start with a fresh database");
    Store left(path), right(path);
    auto access = left.access();
    check(access.administration(owner,"Ernest").at("secretInputs").empty(),"fresh protected input table is not empty");
    const SecretRef ref{"fixture-token"};
    std::barrier start(2);
    auto request = [&](Store& store) {
        start.arrive_and_wait();
        return store.access().requestSecret("Ernest",ref);
    };
    auto other = std::async(std::launch::async,request,std::ref(right));
    const auto id = request(left);
    check(id == other.get(),"repeated secret request returned a different ID");
    check(right.access().requestSecret("Reviewer",ref) != id,"pending secret uniqueness crossed Bot boundaries");
    denied([&]{ access.submitSecret(owner,"Ernest",id,"nest"); });
    access.submitSecret(owner,"Ernest",id,credential);
    check(access.administration(owner,"Ernest").at("secretInputs") ==
          Json::array({{{"id",id},{"name",ref.name},{"status","fulfilled"}}}),"duplicate request remained after fulfillment");
    denied([&]{ access.requestSecret("Ernest",ref); });
}

void exercise(const std::filesystem::path& dir) {
    auto path=dir/"state.db";
    Id request;
    {
        Store store(path);
        auto access=store.access();
        check(access.grants("Ernest").empty(),"fresh grants");
        request=access.request("Ernest",readRepo);
        check(!access.permits("Ernest",readRepo,true),"pending request granted");
    }
    Store store(path);
    auto access=store.access();
    Intake ernest{"Ernest","read","env","project"}, reviewer{"Reviewer","review","env","project"};
    auto child=store.submit(ernest), peerChild=store.submit(reviewer);
    ToolModel model;
    Adapter t3;
    Runtime runtime(store,model,t3,[](std::string_view){return BotNotes{};});
    unsigned message=0;
    auto turn=[&](std::string name, Json args=Json::object(),std::string text="acknowledged") {
        model.next={std::move(name),std::move(args),""};
        store.receive(localOwner,ernest,std::to_string(++message),text);
        runtime.runConversations();
        return model.result.dump();
    };
    turn("list_grants");
    check(turn("request_access",{{"resourceKind","repository"},{"resourceId","other"},{"operation","write"}}).find("pending")!=std::string::npos,
          "request tool missing");
    check(!access.permits("Ernest",{"repository","other","write"},true),"request tool granted scope");
    ernest.config["services"]={"github"};
    ernest.config["mcpServers"]={"docs"};
    auto connections=turn("list_connections");
    check(connections.find("github")!=std::string::npos && connections.find("docs")!=std::string::npos,"connection metadata");
    access.decide(owner,"Ernest",access.request("Ernest",{"web","docs","fetch"}),true);
    check(!access.permitsTask(child,readRepo,true),"restart, chat or unrelated approval granted read");
    access.personGrant(owner,"Ernest","alice",Role::admin,GrantChange::grant);
    access.personGrant(owner,"Ernest","bob",Role::view,GrantChange::grant);
    denied([&]{access.personGrant(viewer,"Ernest","bob",Role::admin,GrantChange::grant);});
    denied([&]{access.decide(admin,"Ernest",request,true);});
    denied([&]{access.resourceGrant(admin,"Ernest",readRepo,GrantChange::grant);});
    access.personScope(owner,"alice",readRepo,GrantChange::grant);
    access.decide(admin,"Ernest",request,true);
    check(access.permitsTask(child,readRepo,true) && !access.permitsTask(child,writeRepo,true) &&
          !access.permitsTask(peerChild,readRepo,true) && !access.permitsTask(child,readRepo,false),"scope or upstream denial");
    denied([&]{access.decide(owner,"Ernest",request,true);});
    access.resourceGrant(owner,"Reviewer",readRepo,GrantChange::grant);
    access.resourceGrant(owner,"Ernest",readRepo,GrantChange::revoke);
    check(!access.permitsTask(child,readRepo,true) && access.permitsTask(peerChild,readRepo,true),"live revocation");
    access.resourceGrant(owner,"Ernest",Scope::sessionRead("env","s"),GrantChange::grant);
    for (auto op:{"steer","interrupt","stop","modify"}) check(!access.permitsTask(child,{"session",Scope::sessionRead("env","s").resource,op},true),"Session control");
    check(turn("request_secret_input",{{"name","repo-token"}}).find("pending")!=std::string::npos,"protected input request");
    auto metadata=access.administration(owner,"Ernest");
    Id input=metadata.at("secretInputs").at(0).at("id");
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
    access.resourceGrant(owner,"Ernest",{"secret","repo-token","use"},GrantChange::grant);
    access.useSecret(child,{"repo-token"},true,[](auto v){check(v==credential,"resolution");});
    denied([&]{access.useSecret(peerChild,{"repo-token"},true,[](auto){});});
    try {
        access.useSecret(child,{"repo-token"},true,[](auto v){throw Conflict(std::string(v));});
        throw std::runtime_error("adapter exception disappeared");
    }
    catch(const Conflict& e){check(std::string(e.what()).find(credential)==std::string::npos,"adapter error leaked");}
    check(turn("check_environment_readiness",{{"environmentId","env"}}).find("/settings/Ernest")!=std::string::npos,"missing setup link");
    access.resourceGrant(owner,"Ernest",{"environment","env","launch"},GrantChange::grant);
    check(turn("check_environment_readiness",{{"environmentId","env"}}).find("\"ready\":true")!=std::string::npos,"readiness");
    t3.connected = false;
    check(turn("check_environment_readiness").find("T3 sign-in")!=std::string::npos,"OAuth denial");
    t3.connected = true;
    check(turn("update_bot_settings",{{"environmentId","env"},{"runTimeoutSeconds",123}}).find("configure access required")!=std::string::npos,
          "settings mutation accepted without a grant");
    access.resourceGrant(owner,"Ernest",{"environment","env","configure"},GrantChange::grant);
    turn("update_bot_settings",{{"environmentId","env"},{"runTimeoutSeconds",123}});
    access.resourceGrant(owner,"Ernest",{"environment","env","inspect"},GrantChange::grant);
    check(turn("list_connections").find("\"runTimeoutSeconds\":123")!=std::string::npos,"settings not effective");
    for (auto op:{"create_bot","rename_bot"}) {
        access.resourceGrant(owner,"Ernest",{"bot_admin","Target",op},GrantChange::grant);
        turn("request_bot_change",{{"operation",op},{"targetBotId","Target"},{"name","New"}});
    }
    Store reopened(path);
    auto changes=reopened.access().administration(owner,"Ernest").at("botChanges");
    check(changes.size()==2 && changes.at(0).at("status")=="pending","durable pending changes");
    denied([&]{access.reviewBotChange(viewer,"Ernest",changes.at(0).at("id"),true,[](const BotChange&){});});
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
    check(model.result.at("turn").at(0).at("result").at("names").empty(),"other Bot's secret names visible");
    check(access.redactSecrets("Reviewer",Json(credential))==credential &&
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
        concurrentApproval(directory/"race.db");
        freshLaunch(directory/"fresh.db");
        runningSessions(directory/"peers.db");
        freshPendingSecretRequests(directory/"secret-requests"/"state.db");
        exercise(directory);
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);
}
