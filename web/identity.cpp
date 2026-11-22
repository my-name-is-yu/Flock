#include "flock/web.hpp"
#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flockv0 {
namespace {
bool bindingId(std::string_view id) {
    return !id.empty() && id.size() <= 1024 && id.front() != ' ' && id.back() != ' ' &&
           std::none_of(id.begin(),id.end(),[](unsigned char c) { return c < 32 || c == 127; });
}
}

void configureWebIdentities(Store& store, const std::filesystem::path& path, WebCallbacks& callbacks) {
    const int fd = open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    struct Close {int fd; ~Close() {if (fd>=0) close(fd);} } closeFile{fd};
    struct stat info{};
    if (fd<0 || fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_uid!=geteuid() ||
        (info.st_mode & 07777)!=0600 || info.st_size<=0 || info.st_size>65536)
        throw Conflict("Web identities require an owner-only regular file");
    std::string data(info.st_size,'\0');
    size_t offset=0;
    while (offset<data.size()) {
        auto count=read(fd,data.data()+offset,data.size()-offset);
        if (count<0 && errno==EINTR) continue;
        if (count<=0) throw Conflict("Cannot read Web identities");
        offset+=static_cast<size_t>(count);
    }
    const auto entries=Json::parse(data,nullptr,false);
    if (!entries.is_array()) throw Conflict("Invalid Web identities");
    std::map<std::string,Person,std::less<>> people;
    std::map<std::string,std::map<std::string,std::string,std::less<>>,std::less<>> t3People;
    std::vector<std::string> roots;
    for (auto& item:entries) {
        try {
            const auto credential=item.at("authorization").get<std::string>();
            Person principal{item.at("person").get<std::string>(),item.value("adminRoot",false)};
            if (principal.id.empty() || credential.size()<16 ||
                (!credential.starts_with("Basic ") && !credential.starts_with("Bearer ")) ||
                credential.find_first_of("\r\n")!=credential.npos || !people.emplace(credential,principal).second)
                throw Conflict("Invalid Web identity");
            if (const auto bindings = item.find("t3People"); bindings != item.end()) {
                if (!bindings->is_object() || !bindingId(principal.id)) throw Conflict("Invalid T3 person binding");
                for (const auto& [environment,value] : bindings->items()) {
                    const auto& ref = value.get_ref<const std::string&>();
                    if (!bindingId(environment) || !bindingId(ref)) throw Conflict("Invalid T3 person binding");
                    const auto [entry,fresh] = t3People[environment].try_emplace(ref,principal.id);
                    if (!fresh && entry->second != principal.id) throw Conflict("Ambiguous T3 person binding");
                }
            }
            if (principal.adminRoot) roots.push_back(std::move(principal.id));
        } catch (const Json::exception&) { throw Conflict("Invalid Web identity"); }
    }
    store.setRootPeople(std::move(roots));
    std::map<std::string,Person,std::less<>> peopleById;
    for (const auto& [credential, person] : people) {
        const auto [prior, inserted] = peopleById.try_emplace(person.id,person);
        if (!inserted) prior->second.adminRoot &= person.adminRoot;
    }
    callbacks.configuredPerson = [people=std::move(peopleById)](std::string_view id) -> std::optional<Person> {
        const auto found = people.find(id);
        return found == people.end() ? std::nullopt : std::optional(found->second);
    };
    callbacks.authenticate = [people=std::move(people)](std::string_view authorization) -> Person {
        auto found=people.find(authorization);
        return found==people.end() ? Person{} : found->second;
    };
    callbacks.bindT3Person = {};
    if (!t3People.empty()) callbacks.bindT3Person = [bound=std::move(t3People)](
        std::string_view environment, std::string_view ref, const Person& actor) {
        const auto target = bound.find(environment);
        if (target == bound.end()) return false;
        const auto person = target->second.find(ref);
        return person != target->second.end() && person->second == actor.id;
    };
}
}
