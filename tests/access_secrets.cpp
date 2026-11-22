#include "flock/access.hpp"
#include <filesystem>
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
    access.resourceGrant(owner,"Ernest",readRepo,false);
    for (auto bot : {"Ernest","Reviewer"}) access.resourceGrant(owner,bot,{"environment","env","launch"},false);
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
        access.executeSessionResource("env",adapter.running.at(bot),readRepo,[&]{ return upstream; },[&]{completed.push_back(bot);});
    };
    read("Ernest");
    denied([&]{read("Reviewer");});
    access.resourceGrant(owner,"Reviewer",readRepo,false);
    read("Reviewer");
    denied([&]{read("Reviewer",false);});
    access.resourceGrant(owner,"Ernest",readRepo,true);
    denied([&]{read("Ernest");});
    read("Reviewer");
    check(completed == std::vector<std::string>{"Ernest","Reviewer","Reviewer"},"completed actions after revocation");
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
        store.receive(ernest,std::to_string(++message),text);
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
    access.personGrant(owner,"Ernest","alice",Role::admin,false);
    access.personGrant(owner,"Ernest","bob",Role::view,false);
    denied([&]{access.personGrant(viewer,"Ernest","bob",Role::admin,false);});
    denied([&]{access.decide(admin,"Ernest",request,true);});
    denied([&]{access.resourceGrant(admin,"Ernest",readRepo,false);});
    access.personScope(owner,"alice",readRepo,false);
    access.decide(admin,"Ernest",request,true);
    check(access.permitsTask(child,readRepo,true) && !access.permitsTask(child,writeRepo,true) &&
          !access.permitsTask(peerChild,readRepo,true) && !access.permitsTask(child,readRepo,false),"scope or upstream denial");
    denied([&]{access.decide(owner,"Ernest",request,true);});
    access.resourceGrant(owner,"Reviewer",readRepo,false);
    access.resourceGrant(owner,"Ernest",readRepo,true);
    check(!access.permitsTask(child,readRepo,true) && access.permitsTask(peerChild,readRepo,true),"live revocation");
    access.resourceGrant(owner,"Ernest",{"session","s","read"},false);
    for (auto op:{"steer","interrupt","stop","modify"}) check(!access.permitsTask(child,{"session","s",op},true),"Session control");
    check(turn("request_secret_input",{{"name","repo-token"}}).find("pending")!=std::string::npos,"protected input request");
    auto metadata=access.administration(owner,"Ernest");
    Id input=metadata.at("secretInputs").at(0).at("id");
    denied([&]{access.submitSecret(viewer,"Ernest",input,credential);});
    access.submitSecret(owner,"Ernest",input,credential);
    struct stat st{};
    check(!stat((dir/"secrets").c_str(),&st) && (st.st_mode&0777)==0700,"directory permissions");
    check(!stat((dir/"secrets"/"repo-token").c_str(),&st) && (st.st_mode&0777)==0600,"secret permissions");
    check(turn("list_secrets",Json::object(),credential).find("repo-token")!=std::string::npos,"names unavailable");
    access.resourceGrant(owner,"Ernest",{"secret","repo-token","use"},false);
    access.useSecret(child,{"repo-token"},true,[](auto v){check(v==credential,"resolution");});
    denied([&]{access.useSecret(peerChild,{"repo-token"},true,[](auto){});});
    try {access.useSecret(child,{"repo-token"},true,[](auto v){throw Conflict(std::string(v));});}
    catch(const Conflict& e){check(std::string(e.what()).find(credential)==std::string::npos,"adapter error leaked");}
    check(turn("check_environment_readiness",{{"environmentId","env"}}).find("/settings/Ernest")!=std::string::npos,"missing setup link");
    access.resourceGrant(owner,"Ernest",{"environment","env","launch"},false);
    check(turn("check_environment_readiness",{{"environmentId","env"}}).find("\"ready\":true")!=std::string::npos,"readiness");
    t3.connected = false;
    check(turn("check_environment_readiness").find("T3 sign-in")!=std::string::npos,"OAuth denial");
    t3.connected = true;
    access.resourceGrant(owner,"Ernest",{"environment","env","configure"},false);
    turn("update_bot_settings",{{"environmentId","env"},{"runTimeoutSeconds",123}});
    check(store.task(store.submit(ernest)).at("config").at("runTimeoutSeconds")==123,"settings not effective");
    for (auto op:{"create_bot","rename_bot"}) {
        access.resourceGrant(owner,"Ernest",{"bot_admin","Target",op},false);
        turn("request_bot_change",{{"operation",op},{"targetBotId","Target"},{"name","New"}});
    }
    Store reopened(path);
    auto changes=reopened.access().administration(owner,"Ernest").at("botChanges");
    check(changes.size()==2 && changes.at(0).at("status")=="pending","durable pending changes");
    denied([&]{access.reviewBotChange(viewer,"Ernest",changes.at(0).at("id"),true);});
    check(store.conversation("Ernest").dump().find(credential)==std::string::npos &&
          access.administration(owner,"Ernest").dump().find(credential)==std::string::npos,"value in conversation or audit");
    check(chmod((dir/"secrets"/"repo-token").c_str(),0644)==0,"fixture mode");
    denied([&]{access.useSecret(child,{"repo-token"},true,[](auto){});});
    check(chmod((dir/"secrets"/"repo-token").c_str(),0600)==0,"restore fixture mode");
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
    try { runningSessions(std::filesystem::path(tmp)/"peers.db"); exercise(tmp); }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';std::filesystem::remove_all(tmp);return 1;}
    std::filesystem::remove_all(tmp);
}
