#pragma once

namespace flockv0::commit_check {
inline constexpr auto legacy = "git rev-parse --verify HEAD";
inline constexpr auto before = "git -c flock.check=before rev-parse --verify HEAD";
inline constexpr auto after = "git -c flock.check=after rev-parse --verify HEAD";
}
