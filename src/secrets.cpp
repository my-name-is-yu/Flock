#include "storage.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flockv0 {
namespace {
constexpr std::string_view redactionPlaceholder = "[redacted]";

struct File {
    int fd;
    explicit File(int value) : fd(value) {}
    File(const File&) = delete;
    File(File&& other) noexcept : fd(std::exchange(other.fd,-1)) {}
    ~File() { if (fd >= 0) close(fd); }
};
void verify(int fd, bool directory) {
    struct stat info{};
    if (fstat(fd,&info) || info.st_uid != geteuid() || (info.st_mode & 07777) != (directory ? 0700 : 0600) ||
        !(directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink==1)) throw Conflict("Unsafe secret vault permissions");
}
File openDirectory(int stateDirectory, bool create) {
    if (create) {
        if (mkdirat(stateDirectory,"secrets",0700)==0) {
            if (fsync(stateDirectory)) throw Conflict("Cannot persist vault directory");
        } else if (errno!=EEXIST) throw Conflict("Cannot create secret vault");
    }
    File directory(openat(stateDirectory,"secrets",O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_DIRECTORY));
    verify(directory.fd,true);
    return directory;
}

template<class Matches>
bool anyJsonText(const Json& value, const Matches& matches) {
    if (value.is_string()) return matches(value.get_ref<const std::string&>());
    if (!value.is_structured()) return false;
    for (const auto& item : value.items())
        if ((value.is_object() && matches(item.key())) || anyJsonText(item.value(),matches)) return true;
    return false;
}
}

bool SecretRef::valid() const {
    return !name.empty() && name.size() <= 120 && name.front() != '.' &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == name.npos;
}

SecretStore::SecretStore(const std::filesystem::path& stateDirectory) {
    const auto path = stateDirectory.empty() ? std::filesystem::path(".") : stateDirectory;
    std::filesystem::create_directories(path);
    stateDirectory_ = open(std::filesystem::canonical(path).c_str(),O_RDONLY|O_CLOEXEC|O_DIRECTORY|O_NOFOLLOW);
    if (stateDirectory_<0) throw Conflict("Cannot open vault parent directory");
}

SecretStore::~SecretStore() { close(stateDirectory_); }

void SecretStore::insert(const SecretRef& ref, std::string_view value) {
    if (!ref.valid() || value.empty() || value.size() > 65536) throw Conflict("Invalid secret name or size");
    auto directory = openDirectory(stateDirectory_,true);
    const auto staging = ".pending-" + actionId();
    File temp(openat(directory.fd,staging.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600));
    if (temp.fd < 0) throw Conflict("Cannot store secret");
    struct Cleanup {
        int directory;
        const std::string& name;
        ~Cleanup() { unlinkat(directory,name.c_str(),0); }
    } cleanup{directory.fd,staging};
    verify(temp.fd,false);
    auto bytes = value;
    while (!bytes.empty()) {
        auto count = write(temp.fd,bytes.data(),bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw Conflict("Cannot store secret");
        bytes.remove_prefix(count);
    }
    if (fsync(temp.fd)) throw Conflict("Cannot persist secret");
    if (renameat(directory.fd,staging.c_str(),directory.fd,ref.name.c_str()))
        throw Conflict("Cannot publish secret");
    if (fsync(directory.fd)) throw Conflict("Cannot persist secret directory");
}

std::string SecretStore::read(const SecretRef& ref) const {
    if (!ref.valid()) throw Conflict("Invalid secret reference");
    auto directory = openDirectory(stateDirectory_,false);
    File file(openat(directory.fd,ref.name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    verify(file.fd,false);
    std::string result;
    char buffer[4096];
    for (;;) {
        auto count = ::read(file.fd,buffer,sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || result.size() > 65536) throw Conflict("Cannot read secret");
        if (!count) break;
        result.append(buffer,static_cast<std::size_t>(count));
    }
    if (result.empty()) throw Conflict("Empty secret");
    return result;
}

SecretRedactor::SecretRedactor(std::vector<std::string> values) : values_(std::move(values)) {
    std::sort(values_.begin(),values_.end(),[](const auto& a,const auto& b){return a.size()>b.size();});
}

std::string SecretRedactor::text(std::string_view source, bool truncated) const {
    if (values_.empty()) return std::string(source);
    std::string out;
    for (size_t i=0; i<source.size();) {
        const auto tail = source.substr(i);
        const auto found = std::find_if(values_.begin(),values_.end(),[&](const auto& value) {
            return tail.starts_with(value) || (truncated && value.starts_with(tail));
        });
        if (found == values_.end()) out += source[i++];
        else { out += redactionPlaceholder; i += found->size(); }
    }
    if (std::any_of(values_.begin(),values_.end(),[&](const auto& v){return out.find(v)!=out.npos;})) out.clear();
    return out;
}

Json SecretRedactor::payload(const Json& input, bool truncated, std::size_t textLimit) const {
    if (values_.empty() && textLimit == std::string_view::npos) return input;
    auto scrub = [&](auto&& self, const Json& item) -> Json {
        if (item.is_string()) return bounded(text(item.get_ref<const std::string&>(),truncated),textLimit);
        if (item.is_array()) {
            auto result = Json::array();
            for (auto& entry : item) result.push_back(self(self,entry));
            return result;
        }
        if (item.is_object()) {
            auto result = Json::object();
            for (auto& [key,value] : item.items()) result[text(key)] = self(self,value);
            return result;
        }
        return item;
    };
    return scrub(scrub,input);
}

Json SecretRedactor::record(Json data, Fields fields) const {
    if (values_.empty()) return data;
    auto visit = [&](auto&& self, Json& value) -> void {
        if (value.is_string()) value = text(value.get_ref<const std::string&>());
        else if (value.is_array()) {
            for (auto& child : value) self(self,child);
        } else if (value.is_object()) {
            for (auto& [key,child] : value.items()) {
                if (fields==Fields::protocol && child.is_string() && (key=="kind" || key=="tool" || key=="state" || key=="role" || key=="operation" ||
                    key=="resourceKind" || key=="status" || key=="effect" || key=="route")) continue;
                self(self,child);
            }
        }
    };
    visit(visit,data);
    return data;
}

void SecretRedactor::requirePublic(const Json& metadata) const {
    if (record(metadata,Fields::external) != metadata) throw Conflict("Secret values cannot be used as metadata");
}

bool SecretRedactor::containsProtected(const Json& value) const {
    return !values_.empty() && anyJsonText(value,[&](std::string_view text) {
        return std::ranges::any_of(values_,[&](const auto& secret) { return text.find(secret) != text.npos; });
    });
}

bool SecretRedactor::mayContainLegacyRedaction(const Json& value) {
    return anyJsonText(value,[](std::string_view text) { return text.empty() || text.find(redactionPlaceholder) != text.npos; });
}

static SecretRedactor readRedactor(Statement rows, const SecretStore& secrets, std::vector<std::string> values) {
    while (rows.next()) values.push_back(secrets.read({rows.string(0)}));
    return SecretRedactor(std::move(values));
}

void Store::protectGitCredential(std::string value) {
    if (value.empty()) return;
    std::lock_guard lock(p_->mutex);
    if (std::ranges::find(p_->gitCredentials,value) != p_->gitCredentials.end()) return;
    auto epoch = p_->query("SELECT random()");
    epoch.next();
    p_->gitCredentials.push_back(std::move(value));
    p_->gitRedactionEpoch = epoch.number(0);
}

SecretRedactor Store::Impl::redactor(std::string_view bot, Json* revision) const {
    std::lock_guard lock(mutex);
    auto rows = query(R"SQL(
SELECT file,id FROM protected_inputs p WHERE p.status='fulfilled'
 AND (p.bot=? OR EXISTS (SELECT 1 FROM grant_audit_log a WHERE a.bot=? AND a.action IN ('approve','grant')
      AND json_extract(a.scope,'$.resourceKind')='secret' AND json_extract(a.scope,'$.resourceId')=p.name))
 ORDER BY id
)SQL",{bot,bot});
    auto values = gitCredentials;
    if (revision) *revision = Json::array({gitRedactionEpoch});
    while (rows.next()) {
        values.push_back(secrets.read({rows.string(0)}));
        if (revision) revision->push_back(rows.number(1));
    }
    return SecretRedactor(std::move(values));
}

SecretRedactor Store::Impl::sharedRedactor() const {
    std::lock_guard lock(mutex);
    return readRedactor(query("SELECT file FROM protected_inputs WHERE status='fulfilled'"),secrets,gitCredentials);
}

Json Access::redactSecrets(std::string_view bot, const Json& data) const {
    return db_.redactor(bot).record(data,SecretRedactor::Fields::external);
}
}
