#include "internal.hpp"
#include "flock/text.hpp"
#include "flock/environment.hpp"
#include "flock/json.hpp"
#include "flock/t3_mcp.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace flockv0 {
namespace {
struct File {
    int fd;
    ~File() {
        if (fd >= 0) close(fd);
    }
};

Json runtimeRecord(const std::filesystem::path& path) {
    const File file{open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat info{};
    if (file.fd < 0 || fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0022) ||
        info.st_size <= 0 || info.st_size > 16 * 1024)
        return {};
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = read(file.fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return {};
        offset += static_cast<std::size_t>(n);
    }
    return Json::parse(bytes, nullptr, false);
}

std::uint64_t boundedInteger(const Json& record, const char* key, std::uint64_t maximum) {
    const auto value = record.find(key);
    if (value == record.end() || !value->is_number_integer()) return 0;
    const auto number = value->get<std::uint64_t>();
    return number <= maximum ? number : 0;
}
}

std::optional<LocalT3Environment> discoverLocalT3() {
    const auto* configured = std::getenv(environment::t3Home);
    const auto setting = configured ? trimAscii(configured) : std::string{};
    std::filesystem::path base(setting);
    if (setting.empty() || setting == "~" || setting.starts_with("~/")) {
        const auto* user = std::getenv("HOME");
        if (!user || !*user) return {};
        base = user;
        if (setting.empty())
            base /= ".t3";
        else if (setting.size() > 2)
            base += setting.substr(1);
    }

    const auto record = runtimeRecord(base / "userdata" / "server-runtime.json");
    if (boundedInteger(record, "version", 1) != 1) return {};
    const auto pid = boundedInteger(record, "pid", std::numeric_limits<pid_t>::max());
    const auto port = boundedInteger(record, "port", 65535);
    if (pid == 0 || port == 0 || (kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM)) return {};

    const auto origin = field(record, "origin");
    const auto portSuffix = ":" + std::to_string(port);
    if (origin != "http://127.0.0.1" + portSuffix && origin != "http://localhost" + portSuffix && origin != "http://[::1]" + portSuffix)
        return {};

    try {
        const auto response = t3detail::http(origin + "/.well-known/t3/environment", "GET", {}, {}, 1000);
        if (response.status != 200) return {};
        const auto descriptor = t3detail::parse(response.body);
        auto id = field(descriptor, "environmentId");
        const auto label = field(descriptor, "label");
        const auto version = field(descriptor, "serverVersion");
        const auto recordedVersion = field(record, "serverVersion");
        if (id.empty() || id.size() > 512 || std::ranges::any_of(id, [](unsigned char c) { return c <= 32 || c == 127; }) ||
            label.empty() || label.size() > 512 || version.empty() || (!recordedVersion.empty() && recordedVersion != version))
            return {};
        return LocalT3Environment{std::move(id), origin + "/mcp"};
    } catch (const t3detail::Failure&) {
        return {};
    }
}

}
