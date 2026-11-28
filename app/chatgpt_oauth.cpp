#include "chatgpt.hpp"
#include "flock/digest.hpp"
#include "flock/base64.hpp"
#include "flock/json.hpp"
#include "chatgpt_failure.hpp"

#include <cctz/time_zone.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <thread>

namespace flockv0::app::chatgpt {
namespace {
using namespace std::chrono_literals;
constexpr std::string_view baseScopes = "openid profile email offline_access resource.invoke ";
constexpr std::string_view dynamicClient = "dynamic_agent_client";
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr std::string_view jsonType = "application/json";
constexpr std::string_view formType = "application/x-www-form-urlencoded";

using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;

std::string urlPart(const Url& url, CURLUPart part, unsigned flags = 0) {
    char* value = nullptr;
    if (curl_url_get(url.get(), part, &value, flags) != CURLUE_OK) return {};
    const std::unique_ptr<char, decltype(&curl_free)> result(value, curl_free);
    return result.get();
}

std::string lowercase(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

std::string sameIssuer(const std::string& endpoint) {
    Url url(curl_url(), curl_url_cleanup);
    if (endpoint.find('\0') != endpoint.npos || !url ||
        curl_url_set(url.get(), CURLUPART_URL, endpoint.c_str(), CURLU_DISALLOW_USER) != CURLUE_OK ||
        urlPart(url, CURLUPART_SCHEME) != "https" || lowercase(urlPart(url, CURLUPART_HOST)) != "auth.openai.com" ||
        !urlPart(url, CURLUPART_PORT, CURLU_NO_DEFAULT_PORT).empty())
        return {};
    return urlPart(url, CURLUPART_URL);
}

Json parseJson(std::string_view text) {
    auto json = Json::parse(text, nullptr, false);
    return json.is_discarded() ? Json{} : json;
}

bool successful(const HttpResponse& response) {
    return response.status >= 200 && response.status < 300;
}

std::string encodeUrl64(std::string_view bytes) {
    auto result = encodeBase64(bytes);
    std::replace(result.begin(), result.end(), '+', '-');
    std::replace(result.begin(), result.end(), '/', '_');
    while (result.ends_with('=')) result.pop_back();
    return result;
}

std::string decodeUrl64(std::string_view text, std::size_t byteLimit) {
    std::string base64(text);
    for (auto& c : base64) {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
        else if (alphabet.find(c) == alphabet.npos)
            return {};
    }
    if (base64.size() % 4 == 1) return {};
    while (base64.size() % 4 != 0) base64 += '=';
    try {
        return decodeBase64(base64, byteLimit);
    } catch (const std::invalid_argument&) {
        return {};
    } catch (const std::length_error&) {
        return {};
    }
}

template <std::size_t size> std::array<unsigned char, size> randomness() {
    std::array<unsigned char, size> bytes{};
    if (RAND_bytes(bytes.data(), bytes.size()) != 1) throw ModelError("Could not generate ChatGPT sign-in randomness", true);
    return bytes;
}

std::string_view asBytes(const auto& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string newHost() {
    auto bytes = randomness<16>();
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const auto hex = encodeHex(bytes);
    return "urn:uuid:" + hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20);
}

std::string pkceChallenge(std::string_view verifier) {
    return encodeUrl64(asBytes(sha256(verifier)));
}

std::optional<double> number(const Json& value) {
    if (!value.is_number()) return {};
    const auto result = value.get<double>();
    if (!std::isfinite(result)) return {};
    return result;
}

std::optional<double> refreshAt(const Json& value) {
    if (const auto seconds = number(value)) {
        const auto ms = *seconds * 1000;
        if (std::isfinite(ms)) return ms;
    } else if (value.is_string()) {
        cctz::time_point<std::chrono::milliseconds> utc;
        if (cctz::parse("%E4Y-%m-%d%ET%H:%M:%E*S%Ez", value.get_ref<const std::string&>(), cctz::utc_time_zone(), &utc))
            return utc.time_since_epoch().count();
    }
    return {};
}

bool signatureMatches(std::string_view message, std::string_view signature, const Json& jwk) {
    const auto modulus = decodeUrl64(stringField(jwk, "n"), 1024);
    const auto exponent = decodeUrl64(stringField(jwk, "e"), 8);
    if (modulus.empty() || exponent.empty() || signature.size() != modulus.size()) return false;
    const auto binaryNumber = [](std::string_view text) {
        return std::unique_ptr<BIGNUM, decltype(&BN_free)>(
            BN_bin2bn(reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()), nullptr), BN_free);
    };
    auto n = binaryNumber(modulus), e = binaryNumber(exponent);
    if (!n || !e) return false;
    using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    Key key(nullptr, EVP_PKEY_free);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    const std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> builder(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    const std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> import(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr),
                                                                             EVP_PKEY_CTX_free);
    if (!builder || !import || OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_N, n.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(builder.get(), OSSL_PKEY_PARAM_RSA_E, e.get()) != 1)
        return false;
    const std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(builder.get()), OSSL_PARAM_free);
    EVP_PKEY* imported = nullptr;
    if (!params || EVP_PKEY_fromdata_init(import.get()) != 1 ||
        EVP_PKEY_fromdata(import.get(), &imported, EVP_PKEY_PUBLIC_KEY, params.get()) != 1)
        return false;
    key.reset(imported);
#else
    std::unique_ptr<RSA, decltype(&RSA_free)> rsa(RSA_new(), RSA_free);
    if (!rsa || RSA_set0_key(rsa.get(), n.get(), e.get(), nullptr) != 1) return false;
    n.release();
    e.release();
    key.reset(EVP_PKEY_new());
    if (!key || EVP_PKEY_set1_RSA(key.get(), rsa.get()) != 1) return false;
#endif
    const std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* verification = nullptr;
    return digest && EVP_DigestVerifyInit(digest.get(), &verification, EVP_sha256(), nullptr, key.get()) == 1 &&
           EVP_PKEY_CTX_set_rsa_padding(verification, RSA_PKCS1_PADDING) == 1 &&
           EVP_DigestVerify(digest.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                            reinterpret_cast<const unsigned char*>(message.data()), message.size()) == 1;
}

bool coversPlan(const Json& granted) {
    return granted.is_array() && std::find(granted.begin(), granted.end(), directScope) != granted.end();
}
}

double nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string formEncode(const Form& fields) {
    std::string result;
    constexpr std::string_view unescaped = "*-._";
    constexpr auto hex = "0123456789ABCDEF";
    const auto append = [&](std::string_view text) {
        for (const unsigned char c : text) {
            if (c == ' ')
                result += '+';
            else if (alphabet.find(c) != alphabet.npos || unescaped.find(c) != unescaped.npos)
                result += static_cast<char>(c);
            else {
                result += '%';
                result += hex[c >> 4];
                result += hex[c & 0x0f];
            }
        }
    };
    for (const auto& [name, value] : fields) {
        if (!result.empty()) result += '&';
        append(name);
        result += '=';
        append(value);
    }
    return result;
}

Json discover(const HttpTransport& http, std::stop_token stop, std::chrono::milliseconds timeout) {
    HttpRequest request;
    request.url = std::string(issuer) + "/.well-known/openid-configuration";
    request.timeout = timeout;
    request.stop = stop;
    const auto response = http(request);
    auto document = parseJson(response.body);
    if (!successful(response)) throw remoteFailure("ChatGPT sign-in", response.status, document, response.requestId);
    if (stringField(document, "issuer") != issuer) throw ModelError("Could not verify the OpenAI sign-in discovery document", false);
    for (const auto key : {"authorization_endpoint", "token_endpoint", "jwks_uri"}) {
        auto endpoint = sameIssuer(stringField(document, key));
        if (endpoint.empty()) throw ModelError("Could not verify the OpenAI sign-in discovery document", false);
        document[key] = std::move(endpoint);
    }
    return document;
}

Json exchange(const HttpTransport& http, const Json& discovery, Form params, std::stop_token stop) {
    params.emplace_back("resource", resource);
    HttpRequest request;
    request.url = stringField(discovery, "token_endpoint");
    request.body = formEncode(params);
    request.contentType = formType;
    request.accept = jsonType;
    request.stop = stop;
    const auto response = http(request);
    auto tokens = parseJson(response.body);
    if (!successful(response)) throw remoteFailure("ChatGPT sign-in", response.status, tokens, response.requestId);
    if (!tokens.is_object()) throw ModelError("Invalid ChatGPT token response", false);
    return tokens;
}

Json identity(const HttpTransport& http, const Json& idToken, const std::string& clientId, const Json& discovery,
              const std::optional<std::string>& nonce, double now, std::stop_token stop) {
    const auto invalidToken = [] { return Failure("Invalid ChatGPT ID token", false, "invalid_id_token"); };
    if (!idToken.is_string()) throw invalidToken();
    const auto token = std::string_view(idToken.get_ref<const std::string&>());
    const auto first = token.find('.');
    if (first == token.npos) throw invalidToken();
    const auto second = token.find('.', first + 1);
    if (second == token.npos) throw invalidToken();
    const auto headerBytes = decodeUrl64(token.substr(0, first), 4 * 1024 * 1024);
    const auto claimBytes = decodeUrl64(token.substr(first + 1, second - first - 1), 4 * 1024 * 1024);
    const auto signature = decodeUrl64(token.substr(second + 1), 1024);
    const auto header = parseJson(headerBytes);
    const auto claims = parseJson(claimBytes);
    if (!header.is_object() || !claims.is_object() || signature.empty()) throw invalidToken();

    const auto kid = stringField(header, "kid");
    const auto sub = stringField(claims, "sub");
    auto audience = field(claims, "aud");
    if (!audience.is_array()) audience = Json::array({audience});
    const auto azp = stringField(claims, "azp");
    const auto expires = number(field(claims, "exp"));
    const auto issued = number(field(claims, "iat"));
    const auto notBefore = number(field(claims, "nbf"));
    constexpr double skewSeconds = 5;
    const auto seconds = now / 1000;
    if (stringField(header, "alg") != "RS256" || kid.empty() || stringField(claims, "iss") != issuer || clientId.empty() ||
        std::find(audience.begin(), audience.end(), clientId) == audience.end() ||
        ((claims.contains("azp") || audience.size() > 1) && azp != clientId) || sub.empty() || !expires || *expires <= seconds || !issued ||
        *issued > seconds + skewSeconds || (claims.contains("nbf") && (!notBefore || *notBefore > seconds + skewSeconds)) ||
        (nonce && stringField(claims, "nonce") != *nonce))
        throw Failure("ChatGPT ID token did not match this authorization", false, "invalid_id_token");

    HttpRequest request;
    request.url = stringField(discovery, "jwks_uri");
    request.stop = stop;
    const auto response = http(request);
    const auto document = parseJson(response.body);
    const auto& keys = field(document, "keys");
    if (!successful(response) || !keys.is_array()) throw ModelError("OpenAI verification keys are unavailable", true);
    const auto key = std::find_if(keys.begin(), keys.end(), [&](const Json& item) {
        return stringField(item, "kid") == kid && stringField(item, "kty") == "RSA" &&
               (!item.contains("use") || stringField(item, "use") == "sig") &&
               (!item.contains("alg") || stringField(item, "alg") == "RS256");
    });
    if (key == keys.end()) throw ModelError("OpenAI verification key is unavailable", true);
    if (!signatureMatches(token.substr(0, second), signature, *key))
        throw Failure("ChatGPT ID token signature could not be verified", false, "invalid_id_token");
    return {{"subject", sub}, {"email", stringField(claims, "email")}};
}

Json credentials(const Json& tokens, const Json& previous) {
    auto granted = field(previous, "granted");
    const auto scope = field(tokens, "scope");
    if (scope.is_string()) {
        std::istringstream words(scope.get<std::string>());
        granted = Json::array();
        for (std::string word; words >> word;) granted.push_back(std::move(word));
    } else if (!scope.is_null() || !granted.is_array()) {
        throw ModelError("ChatGPT did not confirm the granted permissions", false);
    }
    Json next = {{"granted", granted}};
    if (coversPlan(granted)) {
        auto access = stringField(tokens, "access_token");
        auto refresh = stringField(tokens, "refresh_token");
        const auto expires = number(field(tokens, "expires_in"));
        const auto expiresAt = nowMillis() + expires.value_or(0) * 1000;
        if (!validAccessToken(access) || refresh.empty() || lowercase(stringField(tokens, "token_type")) != "bearer" || !expires ||
            *expires <= 0 || !std::isfinite(expiresAt))
            throw ModelError("ChatGPT returned incomplete plan credentials", false);
        next["accessToken"] = std::move(access);
        next["refreshToken"] = std::move(refresh);
        next["expiresAt"] = expiresAt;
        if (const auto earliest = refreshAt(field(tokens, "earliest_refresh_at"))) next["earliestRefreshAt"] = *earliest;
    }
    return next;
}

Json authorize(const HttpTransport& http, const BrowserOpener& opener, const Json& active, bool printUrl, bool reconsent,
               bool switchAccount, bool web, std::stop_token stop, const std::function<void(const Json&)>& saveRegistration) {
    auto host = stringField(active, "hostId");
    if (host.empty()) {
        host = newHost();
        if (saveRegistration) {
            auto registration = active;
            registration["hostId"] = host;
            saveRegistration(registration);
        }
    }
    const auto savedClient = stringField(active, "clientId");
    const auto client = switchAccount ? std::string{} : savedClient;
    const auto discovery = discover(http, stop);
    const auto state = encodeUrl64(asBytes(randomness<32>()));
    const auto nonce = encodeUrl64(asBytes(randomness<32>()));
    const auto verifier = encodeUrl64(asBytes(randomness<32>()));
    Form authorization = {
        {"client_id", client.empty() ? std::string(dynamicClient) : client},
        {"ext_agent_host_id", host},
        {"response_type", "code"},
        {"scope", std::string(baseScopes) + std::string(directScope)},
        {"resource", std::string(resource)},
        {"state", state},
        {"nonce", nonce},
        {"code_challenge_method", "S256"},
        {"code_challenge", pkceChallenge(verifier)},
    };
    if (client.empty()) authorization.emplace_back("agent_name_hint", "Flock");
    if (reconsent && !client.empty()) authorization.emplace_back("prompt", "consent");
    const auto approved =
        authorizeBrowser(opener, stringField(discovery, "authorization_endpoint"), std::move(authorization), client, printUrl, web, stop);
    if (saveRegistration && client.empty() && !switchAccount) {
        auto registration = active;
        registration["hostId"] = host;
        registration["clientId"] = approved.clientId;
        saveRegistration(registration);
    }

    const auto tokens = exchange(http, discovery,
                                 {
                                     {"grant_type", "authorization_code"},
                                     {"client_id", approved.clientId},
                                     {"code", approved.code},
                                     {"code_verifier", verifier},
                                     {"redirect_uri", approved.redirectUri},
                                 },
                                 stop);
    const auto idToken = field(tokens, "id_token");
    const auto account = identity(http, idToken, approved.clientId, discovery, nonce, nowMillis(), stop);
    const auto subject = stringField(active, "subject");
    const bool differentSubject = !subject.empty() && account["subject"] != subject;
    if (!switchAccount && differentSubject)
        throw ModelError("Sign in to the ChatGPT account already registered with this Flock home", false);
    if (switchAccount && savedClient == approved.clientId && differentSubject)
        throw ModelError("ChatGPT returned a registration for a different account", false);
    auto next = credentials(tokens);
    if (switchAccount && !coversPlan(next["granted"])) throw ModelError("Approve ChatGPT plan usage to switch accounts", false);
    next.update(account);
    next["hostId"] = host;
    next["clientId"] = approved.clientId;
    next["idToken"] = idToken;
    return next;
}

bool revokeSession(const HttpTransport& http, const std::string& clientId, const std::string& refreshToken) {
    try {
        const auto discovery = discover(http, {}, 4s);
        HttpRequest request;
        request.url = sameIssuer(stringField(discovery, "revocation_endpoint"));
        if (request.url.empty()) return false;
        request.body = formEncode({{"token", refreshToken}, {"token_type_hint", "refresh_token"}, {"client_id", clientId}});
        request.contentType = formType;
        request.timeout = 4s;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (attempt) std::this_thread::sleep_for(300ms);
            try {
                const auto response = http(request);
                if (response.status == 200) return true;
                if (response.status < 500) return false;
            } catch (const ModelError& error) {
                if (!error.transient) return false;
            }
        }
    } catch (const ModelError&) {
    }
    return false;
}
}
