#pragma once

#include <string>
#include <string_view>

namespace flockv0 {
// True when this user owns the directory or regular file, no one else has any permission on it, and a file has one link.
bool ownerOnly(int fd, bool directory);

// Replaces `name` in the directory with `bytes` by writing `temporary` and renaming it, so a reader sees the old
// content or the new. Returns false, with `temporary` removed, when any step fails.
bool replacePrivateFile(int directory, const std::string& temporary, const std::string& name, std::string_view bytes);
}
