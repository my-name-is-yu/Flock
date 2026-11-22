#include "http_fixture.hpp"

namespace flockv0::http_test {
void boardInboxEndpoints(const std::filesystem::path& path) {
    Store store(path);
    const Person owner{"owner",true};
    auto access = store.access();
    access.resourceGrant(owner,"release",{"environment","local-env","launch"},GrantChange::grant);
    access.personGrant(owner,"release","reviewer",Role::decide,GrantChange::grant);
    access.personGrant(owner,"release","reader",Role::view,GrantChange::grant);
    const auto release = store.submit({"release","Draft notes","local-env","release"});
    store.submit({"triage","Private request","linked env","triage"});
    FixedModel model;
    FixedT3 t3;
    Runtime runtime(store,model,t3,{});
    tick(runtime,3);
    t3.observations["thread-release"].kind = Observation::Kind::Question;
    t3.observations["thread-release"].question = {"scope-question",Json::array({{{"id","scope"},{"required",true}}})};
    tick(runtime,2);
    const auto before = store.task(release);
    const auto history = store.history(release);
    for (int i = 0; i < 40; ++i)
        store.receive(owner,{"release","unused","local-env","release"},"new-"+std::to_string(i),"Later message");
    auto cb = callbacks();
    cb.authenticate = [](std::string_view authorization) -> Person {
        if (authorization == "Bearer reviewer") return {"reviewer"};
        if (authorization == "Bearer reader") return {"reader"};
        return {};
    };
    auto server = startWeb(store,0,cb);
    Browser browser(*server);
    Browser::checked(browser.client.Get("/api/board"),401);
    Browser::checked(browser.client.Get("/api/inbox"),401);
    browser.client.set_default_headers({{"Authorization","Bearer reviewer"}});
    const auto js = browser.client.Get("/board-inbox.js");
    const auto css = browser.client.Get("/board-inbox.css");
    check(js && js->status == 200 && css && css->status == 200,"board and inbox assets were not embedded");
    const auto board = browser.get("/api/board").at("tasks");
    check(board.size() == 1 && board.at(0).at("botName") == "Release" &&
          board.at(0).at("doneWhen") == "Summarize user-facing changes" &&
          browser.get("/api/board?bot=release&status=waiting").at("tasks") == board &&
          browser.get("/api/board?bot=triage").at("tasks").empty() &&
          browser.get("/api/board?status=terminal").at("tasks").empty(),
          "board endpoint returned incorrect profile, task, or visibility");
    Browser::checked(browser.client.Get("/api/board?status=active&status=waiting"),400);
    const auto inbox = browser.get("/api/inbox").at("items");
    check(inbox.size() == 1 && inbox.at(0).at("itemKind") == "user_question" &&
          inbox.at(0).at("choices").size() == 2 &&
          inbox.at(0).at("prompt") == "利用者向けの変更だけを載せますか？" &&
          browser.get("/api/inbox?bot=release").at("items") == inbox &&
          store.task(release) == before && store.history(release) == history &&
          browser.get("/api/inbox/history").at("decisions").empty(),
          "inbox GET failed to preserve the question or changed storage");
    auto answer = inbox.at(0);
    answer["decision"] = "answer";
    answer["data"] = {{"text","すべて"}};
    auto origin = browser.headers();
    origin.erase("Origin");
    Browser::checked(browser.client.Post("/api/inbox/decide",origin,answer.dump(),"application/json"),403);
    origin.emplace("Origin","https://example.invalid");
    Browser::checked(browser.client.Post("/api/inbox/decide",origin,answer.dump(),"application/json"),403);
    origin = browser.headers();
    origin.erase("X-Flock-Action");
    Browser::checked(browser.client.Post("/api/inbox/decide",origin,answer.dump(),"application/json"),403);
    auto stale = answer;
    stale["targetGeneration"] = answer.at("targetGeneration").get<Id>() + 1;
    check(browser.post("/api/inbox/decide",stale,409).at("error") == "request_changed",
          "HTTP did not report a changed target");
    stale.erase("targetRevision");
    browser.post("/api/inbox/decide",stale,400);
    browser.client.set_default_headers({{"Authorization","Bearer reader"}});
    const auto requestAnchor = board.at(0).at("conversationEventId").get<Id>();
    const auto waitAnchor = inbox.at(0).at("conversationEventId").get<Id>();
    const auto cursor = "&before=" + std::to_string(waitAnchor + 1);
    const auto latest = browser.get("/api/conversation?bot=release");
    const auto source = browser.get("/api/conversation?bot=release" + cursor).at("events");
    check(latest.at("olderBefore") > waitAnchor &&
          source.front().at("id") == requestAnchor && source.front().at("kind") == "request" &&
          source.back().at("id") == waitAnchor && source.back().at("kind") == "waiting" &&
          source.back().at("data").at("text") == inbox.at(0).at("prompt"),
          "view-only reader could not load the older question with its surrounding history");
    Browser::checked(browser.client.Get("/api/conversation?bot=triage" + cursor),403);
    check(browser.post("/api/inbox/decide",answer,403).at("error") == "access_denied" &&
          store.task(release) == before && store.history(release) == history,
          "denied Inbox decision advanced the request");
    browser.client.set_default_headers({{"Authorization","Bearer reviewer"}});
    browser.post("/api/inbox/decide",answer);
    check(browser.post("/api/inbox/decide",answer,409).at("error") == "already_closed" &&
          browser.get("/api/inbox").at("items").empty() &&
          browser.get("/api/inbox/history").at("decisions").at(0).at("decidedBy") == "reviewer",
          "HTTP approval was applied twice or was missing in decision history");
}
}
