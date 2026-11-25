#include "flock/detail/traffic_recording.hpp"
#include "flock/t3.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <regex>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace flockv0::recording {
namespace {

bool refused(const char* reason) {
    std::fprintf(stderr, "Traffic recording refused: %s\n", reason);
    return false;
}

bool validCommit(std::string_view commit) {
    return commit.size() == 40 && commit.find_first_not_of("0123456789abcdefABCDEF") == commit.npos;
}

bool validRevision(Provider provider, std::string_view revision) {
    static const std::regex t3(R"(v?[0-9]+\.[0-9]+\.[0-9]+([.+-][a-zA-Z0-9.+-]+)?)");
    static const std::regex model(R"([a-zA-Z0-9][a-zA-Z0-9_.-]*)");
    return revision.size() <= 80 && std::regex_match(revision.begin(), revision.end(), provider == Provider::T3 ? t3 : model);
}

bool privateText(std::string_view text) {
    static const std::regex sensitive(
        R"re("(authorization|access[_-]?token|refresh[_-]?token|id[_-]?token|client[_-]?secret|api[_-]?key|password)"\s*:)re"
        R"(|\bBearer[ \t]+[a-z0-9_.-]|\b(sk-[a-z0-9_-]{12}|gh[pousr]_[a-z0-9_]{12}|github_pat_[a-z0-9_]{12}))"
        R"(|\b(xox[baprs]-[a-z0-9-]{12}|(AKIA|ASIA)[a-z0-9]{16}|eyJ[a-z0-9_-]{5,512}\.[a-z0-9_-]{1,2048}\.))"
        R"(|-----BEGIN [A-Z ]{0,32}PRIVATE KEY-----|[a-z0-9_.+%-]@[a-z0-9.-]{1,253}\.[a-z]{2})"
        R"(|(^|[" ])(/|~/|[a-z]:[\\/])|/(Users|home|private|tmp|Volumes)/)",
        std::regex::icase);
    return std::regex_search(text.begin(), text.end(), sensitive);
}

bool containsPrivateData(const Json& value) {
    static const std::regex sensitiveKey(
        R"(authorization|access[_-]?token|refresh[_-]?token|id[_-]?token|client[_-]?secret|api[_-]?key|password)", std::regex::icase);
    if (value.is_string()) return privateText(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key, field] : value.items()) {
            if (std::regex_match(key, sensitiveKey) || privateText(key) || containsPrivateData(field)) return true;
        }
    }
    if (value.is_array()) {
        for (const auto& field : value) {
            if (containsPrivateData(field)) return true;
        }
    }
    return false;
}

std::unique_ptr<Capture> configure(Provider provider, const char* pathVariable) noexcept {
    const auto* path = std::getenv(pathVariable);
    if (!path || !*path) return {};
    const auto* commit = std::getenv("FLOCK_RECORD_COMMIT");
    if (!commit || !validCommit(commit)) {
        refused("FLOCK_RECORD_COMMIT must be a full Git commit SHA");
        return {};
    }
    try {
        const auto* model = provider == Provider::ChatGPT ? std::getenv("FLOCK_RECORD_CHATGPT_MODEL") : "";
        if (provider == Provider::ChatGPT && (!model || !validRevision(provider, model) || privateText(model))) {
            refused("FLOCK_RECORD_CHATGPT_MODEL must name the model selected for capture");
            return {};
        }
        return std::make_unique<Capture>(path, provider, commit, model);
    } catch (...) {
        refused("could not configure capture");
        return {};
    }
}

}

Json redactedShape(const Json& value) {
    if (value.is_string()) return value == "" ? "" : "redacted";
    if (value.is_object()) return Json::object();
    if (value.is_array()) return Json::array();
    if (value.is_number_float()) return 0.0;
    if (value.is_number_integer()) return 0;
    return value;
}

Json literalOrRedacted(const Json& value, std::initializer_list<std::string_view> allowed) {
    for (const auto literal : allowed)
        if (value == literal) return value;
    return redactedShape(value);
}

Json Ids::replace(std::string_view domain, const Json& value) {
    if (!value.is_string() && !value.is_number()) return redactedShape(value);
    if (value == "") return value;

    auto& ids = domains_[std::string(domain)];
    const auto number = ids.try_emplace(value.dump(), ids.size() + 1).first->second;
    if (value.is_string()) return std::string(domain) + "-" + std::to_string(number);
    if (value.is_number_float()) return static_cast<double>(number);
    return number;
}

void Fields::redact(std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) pick(key, redactedShape);
}

void Fields::counters(std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) pick(key, [](const Json& value) { return value.is_number_integer() ? value : redactedShape(value); });
}

void Fields::ids(Ids& ids, std::string_view domain, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) pick(key, [&](const Json& value) { return ids.replace(domain, value); });
}

void Fields::literal(std::string_view key, std::initializer_list<std::string_view> allowed) {
    pick(key, [&](const Json& value) { return literalOrRedacted(value, allowed); });
}

Capture* Capture::configured(Provider provider) noexcept {
    if (provider == Provider::T3) {
        static const auto capture = configure(provider, "FLOCK_RECORD_T3");
        return capture.get();
    }
    static const auto capture = configure(provider, "FLOCK_RECORD_CHATGPT");
    return capture.get();
}

Capture::Capture(std::filesystem::path path, Provider provider, std::string commit, std::string selectedRevision)
    : path_(std::move(path)), provider_(provider), commit_(std::move(commit)), selectedRevision_(std::move(selectedRevision)) {
    if (!validCommit(commit_)) throw std::invalid_argument("Capture requires a full Git commit SHA");
}

Capture::~Capture() {
    if (file_ >= 0) close(file_);
}

bool Capture::append(std::string_view revision, const std::function<Json(Ids&)>& projection) noexcept {
    if (!selectedRevision_.empty() && revision != selectedRevision_) return true;
    try {
        std::lock_guard lock(mutex_);
        if (writeFailed_) return false;
        auto exchange = projection(ids_);
        if (exchange.is_null()) return true;
        if (!validRevision(provider_, revision)) return refused("invalid provider revision");
        const auto revisionKey = provider_ == Provider::T3 ? "t3Version" : "modelSlug";
        if (privateText(revision) || containsPrivateData(exchange)) return refused("potential secret or private data");
        return write(
            Json{{"schema", 1}, {"source", "real"}, {"captureCommit", commit_}, {revisionKey, revision}, {"exchange", std::move(exchange)}}
                .dump() +
            '\n');
    } catch (...) {
        return refused("could not project exchange");
    }
}

bool Capture::write(std::string_view line) {
    if (file_ < 0) file_ = open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (file_ >= 0) {
        const auto start = lseek(file_, 0, SEEK_CUR);
        while (start >= 0 && !line.empty()) {
            const auto written = ::write(file_, line.data(), line.size());
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) break;
            line.remove_prefix(static_cast<std::size_t>(written));
        }
        if (line.empty()) return true;
        if (start >= 0) ftruncate(file_, start);
    }
    writeFailed_ = true;
    return refused("could not write a new private capture file");
}

}
