#pragma once

#include <string>
#include <string_view>

namespace flockv0::commit_check {
inline constexpr auto legacy = "git rev-parse --verify HEAD";
inline constexpr auto before = "git -c flock.check=before rev-parse --verify HEAD";
inline constexpr auto after = "git -c flock.check=after rev-parse --verify HEAD";

inline std::string instructions(std::string instruction, std::string_view commit) {
    instruction += "\n\nVerify the requested commit ";
    instruction += commit;
    instruction += ". As your first command, run exactly `";
    instruction += before;
    instruction += "`. After all work and checks, run exactly `";
    instruction += after;
    instruction += "` as your final command. Run each as a standalone command using a command tool. "
                   "Stop and report a mismatch if HEAD differs. Keep the checkout at the requested commit. "
                   "After the final command, write a report with the checks and their outcomes.";
    return instruction;
}
}
