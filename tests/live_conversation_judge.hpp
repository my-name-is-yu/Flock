#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace flockv0::app {
class ChatGPTModel;
}

namespace flockv0::live_test {
enum class Criterion { CancellationTarget, CompletionColumn, CriteriaInstruction, SplitRequest, CanaryExplanation, StagingInstruction };
enum class Verdict { Match, Mismatch, Ambiguous, ReleaseNotes, LicenseAudit };

struct Judgment {
    Verdict verdict;
    std::string evidence;
};

using Judge = std::function<Judgment(Criterion, std::string_view)>;

Judge liveJudge(app::ChatGPTModel&, const std::string& slug);
Judgment parseJudgment(Criterion, std::string_view source, std::string_view output);
void judgeSelfTest();
}
