#include "storage.hpp"

#include <algorithm>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flockv0 {
namespace {
struct File {
    int fd;
    explicit File(int value) : fd(value) {}
    File(const File&) = delete;
    ~File() { if (fd >= 0) close(fd); }
};
void verify(int fd, bool directory) {
    struct stat info{};
    if (fstat(fd,&info) || info.st_uid != geteuid() || (info.st_mode & 0777) != (directory ? 0700 : 0600) ||
        !(directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode))) throw Conflict("Unsafe secret vault permissions");
}
File openDirectory(const std::filesystem::path& path, bool create) {
    if (create && mkdir(path.c_str(),0700) && errno != EEXIST) throw Conflict("Cannot create secret vault");
    int fd = open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_DIRECTORY);
    if (fd < 0 && errno == ENOENT && !create) return File(-1);
    try { verify(fd,true); } catch (...) { if (fd >= 0) close(fd); throw; }
    return File(fd);
}
}

bool SecretRef::valid() const {
    return !name.empty() && name.size() <= 120 && name.front() != '.' &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == name.npos;
}

SecretStore::SecretStore(std::filesystem::path directory) : directory_(std::move(directory)) {}

void SecretStore::insert(const SecretRef& ref, std::string_view value) {
    if (!ref.valid() || value.empty() || value.size() > 65536) throw Conflict("Invalid secret name or size");
    auto directory = openDirectory(directory_,true);
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
    if (linkat(directory.fd,staging.c_str(),directory.fd,ref.name.c_str(),0) &&
        (errno != EEXIST || read(ref) != value)) throw Conflict("Cannot replace existing secret");
}

Json SecretStore::names() const {
    auto directory = openDirectory(directory_,false);
    auto names = Json::array();
    if (directory.fd < 0) return names;
    DIR* iter = fdopendir(directory.fd);
    if (!iter) throw Conflict("Cannot list secret vault");
    directory.fd = -1;
    struct Close { DIR* value; ~Close() { closedir(value); } } close{iter};
    while (true) {
        errno = 0;
        auto* item = readdir(iter);
        if (!item) {
            if (errno) throw Conflict("Cannot list secret vault");
            break;
        }
        if (SecretRef{item->d_name}.valid()) names.push_back(item->d_name);
    }
    std::sort(names.begin(),names.end());
    return names;
}

std::string SecretStore::read(const SecretRef& ref) const {
    if (!ref.valid()) throw Conflict("Invalid secret reference");
    auto directory = openDirectory(directory_,false);
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

Json SecretStore::redactSecrets(const Json& input) const {
    std::vector<std::string> values;
    for (auto& name : names()) values.push_back(read({name.get<std::string>()}));
    if (values.empty()) return input;
    std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.size()>b.size();});
    auto clean = [&](const std::string& source) {
        std::string out;
        for (size_t i=0; i<source.size();) {
            const auto found = std::find_if(values.begin(),values.end(),[&](const auto& v){return source.compare(i,v.size(),v)==0;});
            if (found == values.end()) out += source[i++];
            else { out += "[redacted]"; i += found->size(); }
        }
        if (std::any_of(values.begin(),values.end(),[&](const auto& v){return out.find(v)!=out.npos;})) out.clear();
        return out;
    };
    auto scrub = [&](auto&& self, const Json& item) -> Json {
        if (item.is_string()) return clean(item.get_ref<const std::string&>());
        if (item.is_array()) {
            auto result = Json::array();
            for (auto& entry : item) result.push_back(self(self,entry));
            return result;
        }
        if (item.is_object()) {
            auto result = Json::object();
            for (auto& [key,value] : item.items()) result[clean(key)] = self(self,value);
            return result;
        }
        return item;
    };
    return scrub(scrub,input);
}
}
