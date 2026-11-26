#pragma once

#include <string_view>

namespace flockv0 {
enum class ApprovalGate { allow, waiting, denied, expired, stale };
enum class ApprovalWait { pause, defer };
std::string_view approvalError(ApprovalGate);
}
