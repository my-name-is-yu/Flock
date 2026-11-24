#include "../model.hpp"
#include "../settings_access.hpp"
#include "fixtures.hpp"
#include "flock/base64.hpp"
#include "flock/board_inbox.hpp"

#include <curl/curl.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#endif

#include <algorithm>
#include <array>
#include <future>
#include <iostream>
#include <map>

namespace {
using namespace flockv0;
using namespace flockv0::app;
using namespace flockv0::app::test;

constexpr std::string_view issuer = "https://auth.openai.com";
constexpr std::string_view resource = "https://api.openai.com/v1";
constexpr std::string_view scopes = "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct";
constexpr std::string_view fixtureCode = "fixture+code/login";

struct Account { std::string client, subject, email; };
const Account first{"first-registration","first-subject","first@example.test"};
const Account second{"second-registration","second-subject","second@example.test"};

std::string base64Url(std::string_view bytes) {
    auto encoded = encodeBase64(bytes);
    std::ranges::replace(encoded,'+','-');
    std::ranges::replace(encoded,'/','_');
    while (encoded.ends_with('=')) encoded.pop_back();
    return encoded;
}

std::string challenge(std::string_view verifier) {
    std::array<unsigned char,32> digest{};
    unsigned size = 0;
    check(EVP_Digest(verifier.data(),verifier.size(),digest.data(),&size,EVP_sha256(),nullptr) == 1 &&
          size == digest.size(),"Could not hash the fixture PKCE verifier");
    return base64Url({reinterpret_cast<const char*>(digest.data()),digest.size()});
}

class SigningKey {
public:
    SigningKey() : key_(nullptr,EVP_PKEY_free) {
        const std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)> context(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free);
        EVP_PKEY* key = nullptr;
        check(context && EVP_PKEY_keygen_init(context.get()) == 1 &&
              EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(),2048) == 1 &&
              EVP_PKEY_keygen(context.get(),&key) == 1,"Could not generate an RSA fixture key");
        key_.reset(key);
    }

    Json jwks() const {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        BIGNUM *modulus = nullptr, *exponent = nullptr;
        const auto gotModulus = EVP_PKEY_get_bn_param(key_.get(),OSSL_PKEY_PARAM_RSA_N,&modulus);
        const std::unique_ptr<BIGNUM,decltype(&BN_free)> n(modulus,BN_free);
        const auto gotExponent = EVP_PKEY_get_bn_param(key_.get(),OSSL_PKEY_PARAM_RSA_E,&exponent);
        const std::unique_ptr<BIGNUM,decltype(&BN_free)> e(exponent,BN_free);
        check(gotModulus == 1 && gotExponent == 1,"Could not export the RSA fixture key");
#else
        const BIGNUM *modulus = nullptr, *exponent = nullptr;
        RSA_get0_key(EVP_PKEY_get0_RSA(key_.get()),&modulus,&exponent,nullptr);
        check(modulus && exponent,"Could not export the RSA fixture key");
#endif
        const auto encode = [](const BIGNUM* value) {
            std::string bytes(BN_num_bytes(value),'\0');
            BN_bn2bin(value,reinterpret_cast<unsigned char*>(bytes.data()));
            return base64Url(bytes);
        };
        const Json jwk = {{"kty","RSA"},{"kid","fixture"},{"n",encode(modulus)},{"e",encode(exponent)}};
        return {{"keys",Json::array({jwk})}};
    }

    std::string sign(const Account& account, std::string_view nonce = {}, bool invalidSignature = false) const {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        Json claims = {{"iss",issuer},{"aud",account.client},{"sub",account.subject},{"email",account.email},
                       {"iat",now},{"exp",now + 600}};
        if (!nonce.empty()) claims["nonce"] = nonce;
        const auto data = base64Url(Json{{"alg","RS256"},{"kid","fixture"}}.dump()) + "." + base64Url(claims.dump());
        const std::unique_ptr<EVP_MD_CTX,decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(),EVP_MD_CTX_free);
        std::string signature(EVP_PKEY_size(key_.get()),'\0');
        auto size = signature.size();
        check(digest && EVP_DigestSignInit(digest.get(),nullptr,EVP_sha256(),nullptr,key_.get()) == 1 &&
              EVP_DigestSign(digest.get(),reinterpret_cast<unsigned char*>(signature.data()),&size,
                             reinterpret_cast<const unsigned char*>(data.data()),data.size()) == 1,
              "Could not sign a fixture ID token");
        signature.resize(size);
        if (invalidSignature) signature.back() ^= 1;
        return data + "." + base64Url(signature);
    }
private:
    std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)> key_;
};

using Form = std::map<std::string,std::string,std::less<>>;
using Curl = std::unique_ptr<CURL,decltype(&curl_easy_cleanup)>;

Curl curlClient() {
    Curl client(curl_easy_init(),curl_easy_cleanup);
    check(client != nullptr,"Could not initialize the fixture browser");
    return client;
}

Form formFields(std::string_view query) {
    auto curl = curlClient();
    const auto decode = [&](std::string_view value) {
        auto encoded = std::string(value);
        std::ranges::replace(encoded,'+',' ');
        int size = 0;
        const std::unique_ptr<char,decltype(&curl_free)> text(
            curl_easy_unescape(curl.get(),encoded.c_str(),static_cast<int>(encoded.size()),&size),curl_free);
        check(text != nullptr,"Could not decode a fixture form field");
        return std::string(text.get(),static_cast<std::size_t>(size));
    };
    Form fields;
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto pair = query.substr(0,ampersand);
        const auto equals = pair.find('=');
        check(equals != pair.npos,"Expected a form parameter");
        check(fields.try_emplace(decode(pair.substr(0,equals)),decode(pair.substr(equals + 1))).second,
              "Unexpected duplicate form parameter");
        if (ampersand == query.npos) break;
        query.remove_prefix(ampersand + 1);
    }
    return fields;
}

void browserCallback(const std::string& redirect, const std::string& state, const std::string& client, bool decline) {
    check(redirect.starts_with("http://127.0.0.1:") && redirect.ends_with("/auth/callback"),
          "The callback should use the loopback listener");
    const auto complete = redirect.substr(0,redirect.rfind('/')) + "/complete";
    const auto url = redirect + "?state=" + state +
        (decline ? "&error=access_denied" : "&code=fixture%2Bcode%2Flogin&client_id=" + client);
    auto curl = curlClient();
    const auto get = [&](const std::string& url, long expectedStatus) {
        curl_easy_setopt(curl.get(),CURLOPT_URL,url.c_str());
        curl_easy_setopt(curl.get(),CURLOPT_PROTOCOLS_STR,"http");
        curl_easy_setopt(curl.get(),CURLOPT_NOPROXY,"*");
        curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT_MS,3000L);
        curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,
            +[](char*, std::size_t size, std::size_t count, void*) { return size * count; });
        long status = 0;
        check(curl_easy_perform(curl.get()) == CURLE_OK &&
              curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status) == CURLE_OK &&
              status == expectedStatus,"Unexpected response from the loopback OAuth listener");
    };
    get(redirect + "?state=unrelated",400);
    get(url,303);
    char* location = nullptr;
    check(curl_easy_getinfo(curl.get(),CURLINFO_REDIRECT_URL,&location) == CURLE_OK && location && location == complete,
          "Callback did not redirect to a URL without authorization data");
    get(complete,200);
}

chatgpt::HttpResponse jsonResponse(long status, const Json& json) {
    return {status,"application/json",{},json.dump()};
}

std::string readBytes(const std::filesystem::path& path) {
    std::ifstream stream(path);
    check(static_cast<bool>(stream),"Could not read the fixture profile");
    return {std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
}

class OAuthFixture {
    Sandbox sandbox_;

public:
    const std::filesystem::path home = sandbox_.path;
    const std::filesystem::path directory = home / "chatgpt";
    const std::filesystem::path profile = directory / "profile.json";

    Account account = first;
    Form authorization;
    bool decline = false, wrongNonce = false, invalidSignature = false, grantPlan = true, jwksAvailable = true;
    long revocationStatus = 200;
    unsigned logins = 0, refreshes = 0, revocations = 0;
    std::string revokedToken, modelAuthorization;
    std::function<void()> beforeExchange;

    ChatGPTModel model() {
        return ChatGPTModel(directory,[this](const auto& request) { return requestToProvider(request); },
                            [this](const auto& url) { return openBrowser(url); });
    }

    Json stored() const { return Json::parse(readBytes(profile)); }

    void seed(Json value) const {
        std::filesystem::create_directories(directory);
        std::filesystem::permissions(directory,std::filesystem::perms::owner_all);
        write(profile,value.dump(2) + '\n');
        std::filesystem::permissions(profile,std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    }

    void expire() const {
        auto value = stored();
        value["expiresAt"] = 1;
        value.erase("earliestRefreshAt");
        seed(std::move(value));
    }

    void browserFinished() {
        if (browser_.valid()) browser_.get();
    }

private:
    SigningKey key_;
    std::future<void> browser_;

    std::optional<pid_t> openBrowser(const std::string& url) {
        constexpr std::string_view prefix = "https://auth.openai.com/authorize?";
        check(url.starts_with(prefix),"The browser used the wrong authorization endpoint");
        authorization = formFields(std::string_view(url).substr(prefix.size()));

        // Give the browser driver a successful platform-opener result while
        // the fixture browser independently navigates to the real listener.
        const auto pid = fork();
        if (pid == 0) _exit(0);
        check(pid > 0,"Could not start the fixture browser opener");
        browser_ = std::async(std::launch::async,browserCallback,authorization.at("redirect_uri"),
                              authorization.at("state"),account.client,decline);
        return pid;
    }

    chatgpt::HttpResponse requestToProvider(const chatgpt::HttpRequest& request) {
        if (request.url == std::string(issuer) + "/.well-known/openid-configuration")
            return jsonResponse(200,{{"issuer",issuer},{"authorization_endpoint","https://auth.openai.com/authorize"},
                {"token_endpoint","https://auth.openai.com/token"},{"jwks_uri","https://auth.openai.com/jwks"},
                {"revocation_endpoint","https://auth.openai.com/revoke"}});
        if (request.url == std::string(issuer) + "/jwks")
            return jwksAvailable ? jsonResponse(200,key_.jwks()) : jsonResponse(503,Json::object());
        if (request.url == std::string(resource) + "/models") {
            modelAuthorization = request.authorization;
            return jsonResponse(200,{{"models",Json::array({{{"slug","fixture"},{"visibility","list"}}})}});
        }
        check(request.contentType == "application/x-www-form-urlencoded","Expected a form-encoded OAuth request");
        const auto form = formFields(request.body);
        check(form.at("client_id") == account.client,"OAuth used the wrong issued client");
        if (request.url == std::string(issuer) + "/revoke") {
            ++revocations;
            revokedToken = form.at("token");
            check(form.at("token_type_hint") == "refresh_token","Sign-out did not revoke a refresh token");
            check(stored().contains("pending"),"The refresh token was removed before remote revocation");
            return {revocationStatus,{},{},{}};
        }
        check(request.url == std::string(issuer) + "/token" && form.at("resource") == resource,
              "Token exchange used the wrong endpoint or resource");
        if (beforeExchange) beforeExchange();
        const auto& grant = form.at("grant_type");
        const auto rotated = grant == "refresh_token";
        std::string nonce;
        if (rotated) {
            ++refreshes;
            check(form.at("refresh_token") == "first-refresh" && !form.contains("scope"),
                  "Refresh did not preserve the existing grant and token");
        } else {
            ++logins;
            check(grant == "authorization_code" && form.at("code") == fixtureCode,"Incorrect OAuth authorization code");
            check(form.at("redirect_uri") == authorization.at("redirect_uri"),"Exchange changed the redirect URI");
            check(authorization.at("code_challenge_method") == "S256" && form.at("code_verifier").size() == 43 &&
                  authorization.at("code_challenge") == challenge(form.at("code_verifier")),"PKCE proof did not match");
            nonce = wrongNonce ? "unrelated" : authorization.at("nonce");
        }
        auto tokens = Json{{"id_token",key_.sign(account,nonce,invalidSignature)},
            {"access_token",rotated ? "rotated-access" : "first-access"},
            {"refresh_token",rotated ? "rotated-refresh" : "first-refresh"},
            {"token_type","Bearer"},{"expires_in",3600},{"earliest_refresh_at","2030-01-01T00:00:00.125Z"}};
        if (!rotated) tokens["scope"] = grantPlan ? std::string(scopes) : "openid profile email";
        return jsonResponse(200,tokens);
    }
};

template<typename Operation>
void expectModelError(Operation operation, std::string_view message, bool transient = false) {
    try { operation(); }
    catch (const ModelError& error) {
        check(std::string_view(error.what()).find(message) != std::string_view::npos && error.transient == transient,
              std::string("Unexpected model failure: ") + error.what());
        return;
    }
    throw std::runtime_error("Expected a ChatGPT model failure");
}

Json registerAccount(OAuthFixture& fixture) {
    const auto status = fixture.model().login(false,false);
    fixture.browserFinished();
    check(status.at("connected") == true && status.at("planUsage") == true && status.at("account") == first.email,
          "CLI sign-in did not connect the ChatGPT plan");
    check(fixture.authorization.at("client_id") == "dynamic_agent_client" &&
          fixture.authorization.at("agent_name_hint") == "Flock" &&
          fixture.authorization.at("scope") == scopes &&
          fixture.authorization.at("resource") == resource &&
          fixture.authorization.at("state").size() == 43 && fixture.authorization.at("nonce").size() == 43,
          "CLI sign-in did not request a fresh dynamic registration");
    const auto profile = fixture.stored();
    check(profile.at("clientId") == first.client && profile.at("subject") == first.subject &&
          profile.at("hostId") == fixture.authorization.at("ext_agent_host_id") &&
          profile.at("hostId").get<std::string>().starts_with("urn:uuid:") &&
          profile.at("earliestRefreshAt") == 1'893'456'000'125LL,"CLI sign-in stored an incomplete profile");
    return profile;
}

void signedIdentity() {
    OAuthFixture fixture;
    const auto profile = registerAccount(fixture);
    const auto bytes = readBytes(fixture.profile);
    const auto tryLogin = [&](std::string_view message) {
        expectModelError([&] { fixture.model().login(false,true); },message);
        fixture.browserFinished();
        check(fixture.authorization.at("client_id") == first.client &&
              fixture.authorization.at("prompt") == "consent" &&
              !fixture.authorization.contains("agent_name_hint") &&
              profile.at("hostId") == fixture.authorization.at("ext_agent_host_id"),
              "Reconsent did not reuse the saved registration");
        check(readBytes(fixture.profile) == bytes,"Failed sign-in modified the active profile");
    };

    fixture.wrongNonce = true;
    tryLogin("did not match this authorization");
    fixture.wrongNonce = false;
    fixture.invalidSignature = true;
    tryLogin("signature could not be verified");
    fixture.invalidSignature = false;
    fixture.account.subject = second.subject;
    tryLogin("already registered with this Flock home");
}

void switchAccountWithCurrentAdministrator() {
    OAuthFixture fixture;
    const auto profile = registerAccount(fixture);
    fixture.seed(profile);
    const auto bytes = readBytes(fixture.profile);
    fixture.account = second;
    const auto unexpectedCommit = [](const auto&) { throw std::runtime_error("A failed switch tried to save a profile"); };

    fixture.decline = true;
    expectModelError([&] { fixture.model().loginWeb(false,{},true,unexpectedCommit); },"declined");
    fixture.browserFinished();
    check(fixture.logins == 1 && readBytes(fixture.profile) == bytes,"A declined switch changed the active profile");

    fixture.decline = false;
    fixture.grantPlan = false;
    expectModelError([&] { fixture.model().loginWeb(false,{},true,unexpectedCommit); },"plan usage to switch accounts");
    fixture.browserFinished();
    check(readBytes(fixture.profile) == bytes,"A switch without plan usage changed the active profile");

    Store store(fixture.home / "flock.sqlite3");
    SettingsAccess access(store,fixture.home,nullptr);
    const auto owner = access.change(localOwner,{{"operation","prepare"}}).at("credential");
    access.change(localOwner,{{"operation","activate"},{"password",owner.at("password")}});
    const auto delegated = access.change(localOwner,{{"operation","invite"},{"person","delegate"},{"adminRoot",true}})
        .at("credential");
    const auto authenticated = access.identities().authenticate(
        "Basic " + encodeBase64("delegate:" + delegated.at("password").get<std::string>()));
    check(authenticated == Person{"delegate",true},"The test did not authenticate an administrator");
    fixture.grantPlan = true;
    fixture.beforeExchange = [&] {
        access.change(localOwner,{{"operation","role"},{"person","delegate"},{"adminRoot",false}});
        check(readBytes(fixture.profile) == bytes,"Web authorization published an unverified candidate");
    };
    rejects<DecisionDenied>([&] {
        fixture.model().loginWeb(false,{},true,[&](const auto& commit) {
            access.commitIfAdministrator(authenticated,commit);
        });
    },"A demoted administrator switched the active ChatGPT account");
    fixture.browserFinished();
    check(readBytes(fixture.profile) == bytes,"Rejected Web commit did not preserve the prior profile bytes");

    fixture.beforeExchange = {};
    const auto status = fixture.model().loginWeb(false,{},true,[&](const auto& commit) {
        access.commitIfAdministrator(localOwner,commit);
    });
    fixture.browserFinished();
    check(status.at("planUsage") == true && status.at("account") == second.email &&
          fixture.stored().at("clientId") == second.client && fixture.stored().at("hostId") == profile.at("hostId"),
          "The current administrator could not publish the new registration");
}

void refreshRecoversWithoutRotatingTwice() {
    OAuthFixture fixture;
    registerAccount(fixture);
    fixture.expire();
    fixture.beforeExchange = [&] {
        expectModelError([&] { fixture.model().webStatus(); },"credentials are busy",true);
    };
    fixture.jwksAvailable = false;
    expectModelError([&] { fixture.model().models(); },"verification keys are unavailable",true);
    auto profile = fixture.stored();
    check(profile.at("refreshToken") == "first-refresh" &&
          profile.at("pending").at("refreshToken") == "rotated-refresh",
          "A JWKS failure lost the rotated refresh token");

    fixture.jwksAvailable = true;
    const auto catalog = fixture.model().models();
    check(catalog == Json::array({{{"slug","fixture"},{"displayName","fixture"}}}) &&
          fixture.modelAuthorization == "Bearer rotated-access","Retry did not use the refreshed access token");
    profile = fixture.stored();
    check(fixture.refreshes == 1 && !profile.contains("pending") && profile.at("refreshToken") == "rotated-refresh",
          "Retry did not commit the pending credentials without refreshing again");
}

void signOutRevokesPendingToken() {
    OAuthFixture fixture;
    const auto original = registerAccount(fixture);
    fixture.jwksAvailable = false;
    for (const auto confirmed : {true,false}) {
        fixture.seed(original);
        fixture.expire();
        fixture.revocations = 0;
        fixture.revocationStatus = confirmed ? 200 : 503;
        expectModelError([&] { fixture.model().models(); },"verification keys are unavailable",true);
        check(fixture.model().signOut().at("revocationConfirmed") == confirmed &&
              fixture.revocations == (confirmed ? 1U : 2U) && fixture.revokedToken == "rotated-refresh",
              "Sign-out did not try revoking the newest refresh token or preserve the revocation result");
        check(fixture.stored() == Json{{"hostId",original.at("hostId")},{"clientId",first.client},{"subject",first.subject},
                                      {"email",first.email},{"signedOut",true},{"revocationConfirmed",confirmed}},
              "Sign-out did not retain the registration while removing every token");
        const auto status = fixture.model().webStatus();
        check(status.at("connected") == false && status.at("planUsage") == false && status.at("registered") == true,
              "Web status lost the signed-out account registration");
    }
}
}

int main() try {
    check(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK,"Could not initialize fixture HTTP");
    signedIdentity();
    switchAccountWithCurrentAdministrator();
    refreshRecoversWithoutRotatingTwice();
    signOutRevokesPendingToken();
    curl_global_cleanup();
    std::cout << "ChatGPT OAuth lifecycle fixtures passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
