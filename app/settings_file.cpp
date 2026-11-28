#include "settings_file.hpp"
#include "flock/file_descriptor.hpp"
#include "flock/private_files.hpp"
#include "flock/json.hpp"
#include "flock/types.hpp"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace flockv0::app {
namespace {
constexpr std::size_t maxSettingsBytes = 512 * 1024;

}

Json readSettingsFile(const std::filesystem::path& path, Json fallback) {
    const FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.get() < 0 && errno == ENOENT) return fallback;
    struct stat info{};
    if (!ownerOnly(file.get(), false) || ::fstat(file.get(), &info) || info.st_size <= 0 ||
        static_cast<std::size_t>(info.st_size) > maxSettingsBytes)
        throw Conflict("Flock settings require a private, owner-only file.");
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(file.get(), bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) throw Conflict("Could not read the saved settings.");
        offset += static_cast<std::size_t>(count);
    }
    auto result = Json::parse(bytes, nullptr, false);
    if (!result.is_object()) throw Conflict("The saved settings are invalid.");
    return result;
}

void writeSettingsFile(const std::filesystem::path& path, const Json& value) {
    const auto bytes = value.dump(2) + '\n';
    if (bytes.size() > maxSettingsBytes) throw Conflict("The settings are too large to save.");
    writePrivateFile(path, bytes);
}

void writePrivateFile(const std::filesystem::path& path, std::string_view bytes) {
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const FileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() < 0 || !replacePrivateFile(directory.get(), ".pending-" + actionId(), path.filename().string(), bytes))
        throw Conflict("Could not save the settings.");
}
}
