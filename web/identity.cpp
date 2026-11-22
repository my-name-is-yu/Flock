#include "flock/web.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flockv0 {
std::function<Person(std::string_view)> webIdentities(const std::filesystem::path& path) {
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
    for (auto& item:entries) {
        try {
            const auto credential=item.at("authorization").get<std::string>();
            Person principal{item.at("person").get<std::string>(),item.value("adminRoot",false)};
            if (principal.id.empty() || credential.size()<16 ||
                (!credential.starts_with("Basic ") && !credential.starts_with("Bearer ")) ||
                credential.find_first_of("\r\n")!=credential.npos || !people.emplace(credential,principal).second)
                throw Conflict("Invalid Web identity");
        } catch (const Json::exception&) { throw Conflict("Invalid Web identity"); }
    }
    return [people=std::move(people)](std::string_view authorization) -> Person {
        auto found=people.find(authorization);
        return found==people.end() ? Person{} : found->second;
    };
}
}
