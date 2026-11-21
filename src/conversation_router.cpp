#include "conversation_router.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <regex>
#include <utility>
#include <variant>

namespace flockv0::conversation {
namespace {
using namespace std::literals;
using Candidates = std::vector<const Task*>;

std::string_view trim(std::string_view text) {
    constexpr auto whitespace = " \t\r\n"sv;
    const auto start = text.find_first_not_of(whitespace);
    return start == text.npos ? ""sv : text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

std::string normalize(std::string_view text) {
    std::string normal;
    constexpr std::array digits{"０"sv, "１"sv, "２"sv, "３"sv, "４"sv, "５"sv, "６"sv, "７"sv, "８"sv, "９"sv};
    while (!text.empty()) {
        const auto digit = std::ranges::find_if(digits, [&](auto glyph) { return text.starts_with(glyph); });
        if (digit != digits.end()) {
            normal += static_cast<char>('0' + (digit - digits.begin()));
            text.remove_prefix(digit->size());
        } else if (text.starts_with("　")) {
            normal += ' ';
            text.remove_prefix("　"sv.size());
        } else {
            const char c = text.front();
            normal += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            text.remove_prefix(1);
        }
    }
    return normal;
}

bool any(std::string_view text, std::initializer_list<std::string_view> terms) {
    return std::ranges::any_of(terms, [&](auto term) { return text.find(term) != text.npos; });
}
bool starts(std::string_view text, std::initializer_list<std::string_view> terms) {
    return std::ranges::any_of(terms, [&](auto term) { return text.starts_with(term); });
}
bool oneOf(std::string_view text, std::initializer_list<std::string_view> terms) {
    return std::ranges::find(terms, text) != terms.end();
}
std::string_view sentence(std::string_view text) {
    text = trim(text);
    for (auto suffix : { "。"sv, "！"sv, "?"sv, "？"sv, "!"sv, "."sv }) {
        if (text.ends_with(suffix)) return trim(text.substr(0, text.size() - suffix.size()));
    }
    return text;
}
bool japanese(std::string_view text) {
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        const auto first = static_cast<unsigned char>(text[i]);
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if ((first == 0xe3 && second >= 0x81 && second <= 0x83) || (first >= 0xe4 && first <= 0xe9))
            return true;
    }
    return false;
}
std::string translate(bool ja, std::string_view en, std::string_view jp) {
    return std::string(ja ? jp : en);
}

std::string shortName(const Task& task) {
    if (!task.name.empty()) return bounded(trim(task.name.substr(0, task.name.find('\n'))), 160);
    return bounded(trim(task.intent.substr(0, task.intent.find('\n'))), 160);
}
std::string_view shortNamePrefix(std::string_view name) {
    for (auto separator : {" は "sv, " を "sv, " の "sv, "は"sv, "を"sv, "の"sv, ":"sv, "："sv}) {
        if (const auto at = name.find(separator); at != name.npos && at > 0) return trim(name.substr(0, at));
    }
    return {};
}
bool refersTo(std::string_view text, std::string_view normalizedName) {
    if (normalizedName.empty()) return false;
    if (text == normalizedName) return true;
    if (any(text, {std::string("「") + std::string(normalizedName) + "」",
                   std::string("\"") + std::string(normalizedName) + "\""})) return true;
    for (std::size_t at = text.find(normalizedName); at != text.npos; at = text.find(normalizedName, at + 1)) {
        const auto after = at + normalizedName.size();
        const auto asciiTaskNameChar = [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        };
        if ((at == 0 || !asciiTaskNameChar(text[at - 1])) &&
            (after == text.size() || !asciiTaskNameChar(text[after]))) return true;
    }
    return false;
}
bool namesTask(std::string_view text, const Task& task) {
    const auto name = normalize(shortName(task));
    return refersTo(text, name) || refersTo(text, shortNamePrefix(name));
}
bool active(const Task& task) {
    return !oneOf(task.state, {"completed", "cancelled", "cancelling"});
}

template<class Predicate>
Candidates findTasks(const Context& context, Predicate predicate) {
    Candidates found;
    for (const auto& task : context.tasks) if (predicate(task)) found.push_back(&task);
    return found;
}
Candidates namedTasks(const Candidates& tasks, std::string_view normal) {
    Candidates named;
    for (const auto* task : tasks) if (namesTask(normal, *task)) named.push_back(task);
    return named;
}

Route chosen(Kind kind, std::string_view segment, const Task* task = nullptr) {
    Route result;
    result.kind = kind;
    result.segmentText = segment;
    if (task) {
        result.taskId = task->id;
        result.generation = task->generation;
        result.revision = task->revision;
        result.waitId = task->waitId;
    }
    return result;
}
Route message(Kind kind, std::string_view segment, std::string text) {
    auto result = chosen(kind, segment);
    result.text = std::move(text);
    return result;
}
Route clarify(std::string_view segment, std::string text, const Candidates& candidates = {}) {
    auto out = message(Kind::clarify, segment, std::move(text));
    for (const auto* task : candidates) out.candidates.push_back(task->id);
    return out;
}
Route chooseTask(std::string_view segment, const Candidates& candidates) {
    const bool ja = japanese(segment);
    return clarify(segment, translate(ja, "Which task do you mean?", "どの作業を指していますか？"), candidates);
}

int positiveInt(std::string_view text) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0 ? value : 0;
}
int ordinal(std::string_view text) {
    static const std::regex number(R"((?:^|[^0-9])([1-9][0-9]?)(案目|番目|つ目)|\b(?:option|proposal|alternative|choice)\s*#?([1-9][0-9]?)(?![0-9]))");
    int found = 0;
    auto add = [&](int value) {
        found = found != 0 && found != value ? -1 : value;
    };
    using Iterator = std::regex_iterator<std::string_view::const_iterator>;
    for (Iterator match(text.begin(), text.end(), number), end; match != end; ++match)
        add(positiveInt((*match)[1].matched ? (*match)[1].str() : (*match)[3].str()));
    constexpr std::array japaneseNumbers{"一"sv, "二"sv, "三"sv, "四"sv, "五"sv, "六"sv, "七"sv, "八"sv, "九"sv, "十"sv};
    constexpr std::array englishNumbers{"first"sv, "second"sv, "third"sv, "fourth"sv, "fifth"sv, "sixth"sv, "seventh"sv, "eighth"sv, "ninth"sv, "tenth"sv};
    for (std::size_t i = 0; i < japaneseNumbers.size(); ++i) {
        const auto jp = std::string(japaneseNumbers[i]);
        const auto en = std::string(englishNumbers[i]) + ' ';
        if (any(text, {jp + "案目", jp + "番目", jp + "つ目", en + "option", en + "proposal", en + "alternative", en + "choice"}))
            add(static_cast<int>(i + 1));
    }
    return found;
}

struct Option {
    int number = 0;
    std::size_t indent = 0;
};
Option numberedLine(std::string_view line) {
    const auto indent = line.find_first_not_of(' ');
    if (indent == line.npos || indent >= 4 || line.find('\t') == 0) return {};
    auto cleaned = trim(line);
    const auto headings = cleaned.find_first_not_of('#');
    if (headings != cleaned.npos && headings > 0 && cleaned[headings] == ' ') cleaned = trim(cleaned.substr(headings));
    for (auto prefix : {"- "sv, "* "sv, "+ "sv, "**"sv, "__"sv}) {
        if (cleaned.starts_with(prefix)) cleaned = trim(cleaned.substr(prefix.size()));
    }
    if (cleaned.starts_with('|')) {
        const auto cell = trim(cleaned.substr(1, cleaned.find('|', 1) - 1));
        if (const auto n = positiveInt(normalize(cell)); n > 0 && n <= 99) return {n, indent};
        cleaned = cell;
    }
    const auto normal = normalize(cleaned);
    static const std::regex label(
        R"(^(?:(?:option|proposal|alternative|choice|案)[ #]*([1-9][0-9]?)(?![0-9])|(?:\(|（)?([1-9][0-9]?)(?:案目|案[：: ]|\.|\)|）|、|:|：|．|　|\s+-)))");
    std::smatch match;
    if (std::regex_search(normal, match, label))
        return {positiveInt(match[1].matched ? match[1].str() : match[2].str()), indent};
    constexpr std::array circled{"①"sv, "②"sv, "③"sv, "④"sv, "⑤"sv, "⑥"sv, "⑦"sv, "⑧"sv, "⑨"sv, "⑩"sv};
    const auto mark = std::ranges::find_if(circled, [&](auto glyph) { return normal.starts_with(glyph); });
    if (mark != circled.end()) return {static_cast<int>(mark - circled.begin()) + 1, indent};
    const auto number = ordinal(normal);
    return number && !normal.empty() && any(normal.substr(0, 9), {"案目", "番目", "つ目"}) ? Option{number, indent} : Option{};
}
struct MissingOption {};
struct AmbiguousOption {};
using OptionExcerpt = std::variant<MissingOption, AmbiguousOption, std::string>;

OptionExcerpt findOptionExcerpt(std::string_view fullText, int wanted) {
    std::size_t start = fullText.npos, end = fullText.size(), indent = 0;
    bool code = false;
    for (std::size_t pos = 0; pos < fullText.size();) {
        const auto newline = fullText.find('\n', pos);
        const auto line = fullText.substr(pos, newline - pos);
        if (starts(trim(line), {"```", "~~~"})) {
            code = !code;
        } else if (!code) {
            const auto option = numberedLine(line);
            if (start == std::string_view::npos && option.number == wanted) {
                start = pos;
                indent = option.indent;
            } else if (start != std::string_view::npos && option.indent <= indent && option.number > 0) {
                if (option.number == wanted) return AmbiguousOption{};
                end = std::min(end, pos);
            }
        }
        if (newline == fullText.npos) break;
        pos = newline + 1;
    }
    if (start == fullText.npos) return MissingOption{};
    return bounded(trim(fullText.substr(start, end - start)), 8192);
}

std::string resultExcerpt(std::string_view text, int numbered) {
    if (!numbered) return bounded(text, 8192);
    auto match = findOptionExcerpt(text, numbered);
    if (auto* excerpt = std::get_if<std::string>(&match)) return std::move(*excerpt);
    return {};
}

bool statusRequest(std::string_view text) {
    return any(text, {"進捗は", "進捗を教", "進捗どう", "進捗状況は", "今の状態", "状態は", "どうなってる", "状況は", "状況を教"}) ||
           starts(text, {"status update", "status of", "progress on", "how is it going", "how is the work", "any progress",
                         "what's the status", "how's it going", "what is the status", "show progress", "check status"}) ||
           oneOf(sentence(text), {"進捗", "進捗状況", "状況", "status", "progress", "update"});
}
bool cancellation(std::string_view text) {
    return any(text, {"止めて", "中止して", "キャンセルして", "取り消して", "を中止", "をキャンセル"}) ||
           starts(text, {"cancel ", "stop ", "please cancel ", "please stop "}) ||
           oneOf(sentence(text), {"中止", "キャンセル", "stop", "cancel"});
}
bool deicticCancellation(std::string_view text) {
    text = sentence(text);
    if (text.ends_with("ください")) text = trim(text.substr(0, text.size() - "ください"sv.size()));
    if (text.ends_with(" please")) text = trim(text.substr(0, text.size() - " please"sv.size()));
    if (text.starts_with("please ")) text = trim(text.substr("please "sv.size()));
    for (auto pointer : {"それを"sv, "それ"sv, "この作業を"sv, "その作業を"sv, "今の作業を"sv,
                         "作業を"sv, "いったん"sv, "一旦"sv}) {
        if (text.starts_with(pointer)) text = trim(text.substr(pointer.size()));
    }
    return oneOf(text, {"止めて", "中止して", "キャンセルして", "取り消して", "中止", "キャンセル", "stop", "cancel",
                        "stop it", "stop that", "stop this", "cancel it", "cancel that", "cancel this",
                        "stop the task", "cancel the task", "stop this task", "cancel this task"});
}
bool cancellationMention(std::string_view text) {
    return any(text, {"止めてと言", "止めてと書", "中止してと言", "と言わない", "止めない", "中止しない", "キャンセルしない",
                      "「止めて」", "「中止して」", "don't stop", "don't cancel", "do not stop", "do not cancel", "say stop", "say cancel"});
}
bool resume(std::string_view text) {
    return any(text, {"再開して", "再開してください", "を再開"}) || starts(text, {"resume ", "restart ", "please resume "}) ||
           oneOf(sentence(text), {"再開", "resume", "restart"});
}
bool deicticResume(std::string_view text) {
    return oneOf(sentence(text), {"再開", "再開して", "再開してください", "中止した作業を再開して", "中止した作業を再開してください",
                                 "それを再開", "それを再開して", "それ再開して", "resume", "restart", "resume it", "resume that",
                                 "resume the stopped task", "restart it"});
}
bool readResult(std::string_view text) {
    return any(text, {"詳しく", "詳細", "説明して", "教えて", "見せて", "根拠", "比較して", "要約して", "とは", "結果は",
                      "elaborate", "details", "explain", "tell me", "show me", "summary", "summarize", "what did"});
}
bool requestWork(std::string_view text) {
    if (oneOf(sentence(text), {"お願い", "お願いします", "調査", "テスト", "実装", "作業", "実行", "変更", "review", "test", "build", "do it"}))
        return false;
    for (auto prefix : {"please "sv, "can you "sv, "could you "sv, "would you "sv, "i want you to "sv, "let's "sv}) {
        if (text.starts_with(prefix)) text = trim(text.substr(prefix.size()));
    }
    return any(text, {"して", "してください", "お願い", "知りたい", "直して", "作って", "書いて", "調べて", "変更を", "見直して",
                      "頼みたい", "採用したい", "始めて", "やり直して", "やって", "してほしい", "実行", "調査", "テスト", "実装"}) ||
           starts(text, {"implement ", "build ", "create ", "write ", "fix ", "make ", "add ", "analyze ", "investigate ",
                         "research ", "test ", "compare ", "review ", "deploy ", "run ", "check ", "update ", "design ",
                         "measure ", "verify ", "document ", "prepare ", "summarize ", "refactor ", "improve ", "use ", "try "}) ||
           any(text, {" implement ", " build ", " to implementation", " go ahead", "proceed with"});
}
bool doWorkOnResult(std::string_view text) {
    return any(text, {"で実装", "を実装して", "で進め", "で作って", "を修正", "直して", "作り直", "反映して", "採用して",
                      "実装して", "implement", "build ", "put into practice", "go ahead", "proceed with", "revise", "fix ",
                      "apply ", "update ", "change ", "rework", "turn it into"});
}
bool explicitlySelectsImplementation(std::string_view text) {
    return any(text, {"で実装して", "を実装して", "で実装を進めて"}) ||
           starts(text, {"implement ", "please implement ", "build option ", "please build option "});
}

std::string statusLine(const Task& task, bool ja) {
    const auto state = std::string_view(task.state);
    std::string_view label = state;
    if (state == "queued") label = ja ? "順番待ち" : "Queued";
    if (state == "running" || state == "deciding" || state == "sending") label = ja ? "作業中" : "Working";
    if (state == "waiting") label = ja ? "確認待ち" : "Waiting for input";
    if (state == "cancelling" || state == "interrupting" || state == "replanning") label = ja ? "停止確認中" : "Stopping";
    if (state == "completed") label = ja ? "完了" : "Complete";
    if (state == "cancelled") label = ja ? "中止" : "Cancelled";
    auto line = "- " + shortName(task) + translate(ja, " — ", " ／ ") + std::string(label);
    if (!task.waitText.empty()) line += "\n  " + bounded(task.waitText, 512);
    return line;
}
Route status(std::string_view segment, std::string_view normal, const Context& context) {
    const bool ja = japanese(segment);
    const auto all = findTasks(context, [](const Task&) { return true; });
    auto tasks = namedTasks(all, normal);
    if (tasks.empty()) tasks = findTasks(context, [](const Task& task) {
        return task.state != "completed" && task.state != "cancelled";
    });
    if (tasks.empty() && !all.empty()) tasks = {all.front()};
    if (tasks.empty()) return message(Kind::status, segment, translate(ja, "There are no tasks for this Bot yet.", "この Bot にはまだ作業がありません。"));
    std::string response;
    for (std::size_t i = 0; i < std::min<std::size_t>(tasks.size(), 10); ++i) {
        if (i > 0) response += '\n';
        response += statusLine(*tasks[i], ja);
    }
    if (tasks.size() > 10) response += translate(ja, "\n… and ", "\nほか ") + std::to_string(tasks.size() - 10) + translate(ja, " more.", "件。");
    auto output = message(Kind::status, segment, std::move(response));
    if (tasks.size() == 1) {
        output.taskId = tasks.front()->id;
        if (oneOf(tasks.front()->state, {"completed", "cancelled"})) {
            const auto result = std::ranges::find(context.rankedResults, output.taskId, &Result::taskId);
            if (result != context.rankedResults.end() && !result->text.empty()) {
                output.source = *result;
                output.source->text = bounded(result->text, 512);
                output.text += "\n\n" + output.source->text;
            }
        }
    }
    return output;
}

Route resultReply(std::string_view segment, const Result& source, int numbered) {
    const bool ja = japanese(segment);
    auto quote = resultExcerpt(source.text, numbered);
    if (quote.empty()) return clarify(segment, translate(ja, "I couldn't identify that option in the referenced response. Which result should I use?",
                                                            "参照した回答からその案を特定できませんでした。どの結果の案を指していますか？"));
    auto out = chosen(Kind::respond, segment);
    out.source = source;
    out.source->text = std::move(quote);
    out.taskId = source.taskId;
    if (ja) {
        out.text = "「" + source.taskName + "」の結果に基づくと、";
        out.text += numbered ? std::to_string(numbered) + "案目の内容は次のとおりです。" : "該当する内容は次のとおりです。";
    } else {
        out.text = "From the result for “" + source.taskName + "”";
        out.text += numbered ? ", here is option " + std::to_string(numbered) + "." : ", here is the relevant text.";
    }
    out.text += "\n\n" + out.source->text;
    return out;
}

std::string_view afterPrefix(std::string_view text, std::initializer_list<std::string_view> prefixes) {
    for (auto prefix : prefixes) if (text.starts_with(prefix)) return trim(text.substr(prefix.size()));
    return {};
}
bool offersAnswer(const Task& task, std::string_view value) {
    if (task.waitReason != "user_question" || !task.waitId || value.empty() || value.size() > 120) return false;
    if (!task.waitChoices.empty()) {
        return std::ranges::any_of(task.waitChoices, [&](const auto& choice) { return value == trim(normalize(choice)); });
    }
    const auto wait = normalize(task.waitText);
    if (!any(wait, {"どちら", "どれ", "選ん", "choose", "which", "or "})) return false;
    const auto offered = std::string(value);
    return any(wait, {"「" + offered + "」", "`" + offered + "`", "\"" + offered + "\"", offered + " と ",
                      offered + " or ", " or " + offered, offered + " / ", " / " + offered});
}
bool explicitBudgetGrant(std::string_view text) {
    return oneOf(sentence(text), {"追加1回", "追加 1 回", "追加1回お願いします", "あと1回追加", "あと1回追加して",
                                  "one more run", "one additional run", "add one more run", "grant one more run", "add one run"});
}
bool mentionsAdditionalRuns(std::string_view text) {
    static const std::regex amount(R"((?:追加\s*[0-9]+\s*回|[0-9]+\s*回(?:分)?追加|additional runs?|more runs?|extra runs?))");
    return std::regex_search(text.begin(), text.end(), amount);
}
bool genericAnswer(std::string_view text) {
    return oneOf(sentence(text), {"はい", "はい、お願いします", "はい、お願い", "いいえ", "うん", "そう", "ok", "okay", "yes", "no", "sure",
                                  "yes please", "yes, please", "了解", "了解です", "それで", "そのまま", "this one", "that one"});
}
std::string chatReply(std::string_view text, bool ja) {
    text = sentence(text);
    if (oneOf(text, {"こんにちは", "こんばんは", "おはよう", "おはようございます", "hello", "hi", "hey"})) {
        return translate(ja, "Hi. What would you like to work on?", "こんにちは。進めたい作業があれば教えてください。");
    }
    if (oneOf(text, {"ありがとう", "ありがとうございます", "助かりました", "お疲れさま", "thanks", "thank you", "got it"})) {
        return translate(ja, "You're welcome.", "どういたしまして。");
    }
    return {};
}
bool unsafeApproval(std::string_view text) {
    return oneOf(sentence(text), {"承認", "承認します", "許可", "許可します", "approve", "approved", "allow", "authorize"}) ||
           starts(text, {"grant permission", "approve the permission", "approve t3", "t3を承認", "t3 の権限", "権限を付与", "権限を許可"});
}

Route waitReply(std::string_view segment, std::string_view normal, const Task& task) {
    const bool ja = japanese(segment);
    if (task.waitReason == "budget" && explicitBudgetGrant(normal)) return chosen(Kind::grantOne, segment, &task);
    if (task.waitReason == "user_question" && !unsafeApproval(normal)) {
        const auto answer = afterPrefix(segment, {"質問への回答は", "質問への回答:", "回答:", "回答：", "回答は", "Answer:", "answer:"});
        auto out = chosen(Kind::answerWait, segment, &task);
        out.text = answer.empty() ? std::string(segment) : std::string(answer);
        return out;
    }
    if (task.waitReason == "budget") {
        return clarify(segment, translate(ja, "To approve the budget request, specify one additional run.", "実行枠を追加する場合は「追加1回」と返信してください。"));
    }
    return clarify(segment, translate(ja, "This item requires its own approval or recovery control.", "この確認には専用の承認または復旧操作が必要です。"));
}

struct Destination { std::string alias; bool ambiguous = false; };
Destination executionEnvironment(std::string_view text, const std::vector<std::string>& configured) {
    const bool ja = japanese(text);
    if (const auto contrast = text.rfind("ではなく"); contrast != text.npos) text.remove_prefix(contrast + "ではなく"sv.size());
    else if (const auto contrast = text.find("instead of"); contrast != text.npos) text = text.substr(0, contrast);
    Destination destination;
    for (const auto& alias : configured) {
        if (alias.empty()) continue;
        const auto name = normalize(alias);
        if (!any(text, {name + "で", name + " で", name + "に変更"}) && !refersTo(text, "on " + name) &&
            !refersTo(text, "in " + name) && !refersTo(text, "on the " + name) && !refersTo(text, "use " + name)) continue;
        if (!destination.alias.empty() && destination.alias != alias) return {{}, true};
        destination.alias = alias;
    }
    const bool cloud = any(text, {"クラウドmacで", "クラウド macで", "クラウドmac で", "クラウド mac で", "クラウドmacに",
                                  "on cloud mac", "on a cloud mac", "on the cloud mac", "use cloud mac", "use a cloud mac", "use the cloud mac"});
    const bool local = any(text, {"ローカルmacで", "ローカル macで", "ローカルmac で", "ローカル mac で", "ローカルmacに",
                                  "on local mac", "on a local mac", "on the local mac", "use local mac", "use a local mac", "use the local mac"});
    if (cloud && local) return {{}, true};
    if (!destination.alias.empty()) return destination;
    if (cloud) return {ja ? "クラウドMac" : "cloud Mac"};
    if (local) return {ja ? "ローカルMac" : "local Mac"};
    return destination;
}
bool criteriaChange(std::string_view text) {
    return any(text, {"完了条件は", "受け入れ条件は", "change the acceptance criteria to", "change the completion criteria to"}) ||
           starts(text, {"acceptance criteria:", "completion criteria:", "done when ", "done when:"}) ||
           (any(text, {"完了条件を", "受け入れ条件を"}) && any(text, {"に変更", "に変え", "にして", "に更新", "に絞"}));
}
bool scopeChange(std::string_view text) {
    return any(text, {"目的は", "対象は", "範囲は", "スコープは", "change the goal to", "change the scope to", "change the target to"}) ||
           (any(text, {"目的を", "対象を", "範囲を", "スコープを"}) && any(text, {"に変更", "に変え", "にして", "に更新", "に絞"}));
}
bool continuation(std::string_view text) {
    return any(text, {"追記", "追って", "補足", "続けて", "続き", "作業にも", "も含めて", "追加して", "追加で", "で計測", "で測定",
                      "形式で", "形式は", "フォーマットは", "で出して", "にして", "を使って", "は main", "はmain", "main で", "mainで", "main branch",
                      "csvで", "csv で", "jsonで", "json で", "markdownで", "もテスト", "も確認",
                      "also include", "while you're", "continue", "keep working", "for this task", "in this task", "in the report",
                      "for the measurement", "on main", "on the main", "in csv", "as csv", "as json"});
}
bool hasUnmatchedTarget(std::string_view text) {
    constexpr auto genericSubjects = std::array{"作業"sv, "今回"sv, "今回は"sv, "この作業"sv, "その作業"sv, "今の作業"sv,
                                                "計測"sv, "測定"sv, "計測環境"sv, "報告"sv, "報告書"sv, "出力"sv, "結果"sv,
                                                "ブランチ"sv, "実行環境"sv, "the task"sv, "this task"sv, "the current task"sv};
    for (auto delimiter : {"は main"sv, "はmain"sv, "の完了条件"sv, "の受け入れ条件"sv, "の目的"sv, "の範囲"sv,
                           "のスコープ"sv, "の対象"sv, "に追記"sv, "へ追記"sv}) {
        const auto at = text.find(delimiter);
        if (at == text.npos || at == 0) continue;
        const auto subject = trim(text.substr(0, at));
        return std::ranges::find(genericSubjects, subject) == genericSubjects.end();
    }
    return starts(text, {"for task ", "for the task ", "in task ", "in the task ", "タスク「", "作業「"});
}
std::string fieldChange(std::string_view segment, std::string_view normal, bool criteria) {
    const auto prefixes = criteria
        ? std::initializer_list<std::string_view>{"完了条件は", "完了条件を", "受け入れ条件は", "受け入れ条件を",
                                                 "acceptance criteria:", "completion criteria:", "done when ", "done when:",
                                                 "change the acceptance criteria to ", "change the completion criteria to "}
        : std::initializer_list<std::string_view>{"目的は", "目的を", "範囲は", "範囲を", "スコープは", "スコープを", "対象は", "対象を",
                                                 "change the goal to ", "change the scope to ", "change the target to "};
    for (auto prefix : prefixes) {
        const auto found = normal.find(prefix);
        if (found == normal.npos) continue;
        const auto literal = segment.find(prefix);
        auto value = sentence(segment.substr((literal == segment.npos ? found : literal) + prefix.size()));
        for (auto suffix : {"に変更してください"sv, "に変更して"sv, "に変更"sv, "に変えて"sv, "にしてください"sv, "にして"sv,
                            "に更新して"sv, "に更新"sv}) {
            if (value.ends_with(suffix)) value = trim(value.substr(0, value.size() - suffix.size()));
        }
        if (!value.empty()) return std::string(value);
    }
    return std::string(segment);
}

const Task* taskById(const Context& context, Id id) {
    const auto found = std::ranges::find(context.tasks, id, &Task::id);
    return found == context.tasks.end() ? nullptr : &*found;
}
const Result* resultForTask(const Context& context, Id id) {
    const auto found = std::ranges::find(context.rankedResults, id, &Result::taskId);
    return found == context.rankedResults.end() ? nullptr : &*found;
}
bool question(std::string_view text) {
    return text.ends_with('?') || text.ends_with("？") ||
           any(text, {"してもいい", "してよい", "できますか", "できる？", "何ですか", "どういう意味", "するには"});
}
bool resultReference(std::string_view text) {
    return any(text, {"前の結果", "前回の結果", "前の回答", "前回の回答", "前回の提案", "先ほどの", "さっきの", "その結果", "あの結果",
                      "previous result", "last result", "last response", "that result", "that proposal", "your result", "above result"});
}
bool pointsAtRecentResult(std::string_view text) {
    return starts(text, {"それで", "その案", "その方向", "それを", "implement that", "implement it", "build that", "go ahead with that",
                         "proceed with that", "explain that", "tell me more"});
}
Candidates targets(const Candidates& qualified, std::string_view normal, Id preferred) {
    if (preferred) {
        const auto target = std::ranges::find_if(qualified, [&](const Task* task) { return task->id == preferred; });
        return target == qualified.end() ? Candidates{} : Candidates{*target};
    }
    const auto named = namedTasks(qualified, normal);
    return named.empty() ? qualified : named;
}
Route resultWork(std::string_view segment, const Context& context, const Result& source, int numbered) {
    const bool ja = japanese(segment);
    auto selected = resultExcerpt(source.text, numbered);
    if (selected.empty()) return clarify(segment, translate(ja, "Which result should I use for the new work?", "どの結果を使って作業しますか？"));
    const auto* task = taskById(context, source.taskId);
    const auto state = task ? std::string_view(task->state) : std::string_view(source.state);
    if (state == "cancelled" || state == "cancelling")
        return clarify(segment, translate(ja, "Would you like to explicitly start new work from that stopped task?", "中止した作業をもとに新しい作業を始めますか？"));
    auto out = chosen(state == "completed" ? Kind::createTask : Kind::appendInstruction, segment, task);
    out.text = segment;
    out.source = source;
    out.source->text = std::move(selected);
    if (out.kind == Kind::createTask) out.taskId = 0;
    return out;
}

Route classify(std::string_view segment, const Context& context, bool independent, Id preferred, const Reply& anchor) {
    const std::string normalized = normalize(segment);
    const std::string_view normal = trim(normalized);
    const bool ja = japanese(segment);
    const auto staleReply = [&] {
        return message(Kind::respond, segment, translate(ja, "That question or response is no longer available. Your reply had no effect.",
                                                          "返信先はすでに閉じているか、参照できません。現在の作業に変更はありません。"));
    };
    if (anchor.kind == Reply::Kind::invalid) return staleReply();
    if (anchor.kind == Reply::Kind::wait) {
        const auto* task = taskById(context, anchor.taskId);
        if (!task || !active(*task) || !anchor.waitId || task->waitId != anchor.waitId) return staleReply();
        if (statusRequest(normal)) return status(segment, normal, context);
        return waitReply(segment, normal, *task);
    }
    if (statusRequest(normal)) return status(segment, normal, context);

    const auto open = findTasks(context, active);
    const auto all = findTasks(context, [](const Task&) { return true; });
    const auto named = namedTasks(all, normal);
    const auto anchoredTask = anchor.result ? anchor.result->taskId : preferred;
    const auto liveTargets = targets(open, normal, anchoredTask);
    const bool namesOnlyClosedTasks = !named.empty() && namedTasks(open, normal).empty();
    if (unsafeApproval(normal))
        return clarify(segment, translate(ja, "Use the approval control for the request you want to approve.", "承認する場合は、対象の確認カードから操作してください。"));
    if (cancellationMention(normal))
        return clarify(segment, translate(ja, "Let me know which task you want to discuss.", "どの作業について話していますか？"));
    if (!independent && cancellation(normal)) {
        const bool anchorMismatch = anchoredTask && !named.empty() &&
                                    std::ranges::none_of(named, [&](const Task* task) { return task->id == anchoredTask; });
        const bool unresolved = namesOnlyClosedTasks || anchorMismatch ||
                                (!anchoredTask && named.empty() && !deicticCancellation(normal));
        if (unresolved || question(normal) || liveTargets.size() != 1) return chooseTask(segment, unresolved ? Candidates{} : liveTargets);
        return chosen(Kind::cancelTask, segment, liveTargets.front());
    }
    if (!independent && resume(normal)) {
        const auto cancelled = findTasks(context, [](const Task& task) { return task.state == "cancelled"; });
        const auto candidates = targets(cancelled, normal, anchoredTask);
        const bool anchorMismatch = anchoredTask && !named.empty() &&
                                    std::ranges::none_of(named, [&](const Task* task) { return task->id == anchoredTask; });
        const bool unresolved = anchorMismatch || (!anchoredTask && named.empty() && !deicticResume(normal)) ||
                                (!named.empty() && namedTasks(cancelled, normal).empty());
        if (unresolved) return chooseTask(segment, {});
        if (question(normal) || candidates.size() != 1)
            return chooseTask(segment, candidates);
        const auto* task = candidates.front();
        auto out = message(Kind::createTask, segment, std::string(segment));
        if (const auto* result = resultForTask(context, task->id)) out.source = *result;
        else out.source = Result{task->id, 0, task->revision, shortName(*task), bounded(task->intent, 8192), task->state};
        return out;
    }

    const int option = ordinal(normal);
    const auto* linkedResult = anchor.result ? &*anchor.result : nullptr;
    if (anchor.kind == Reply::Kind::result && !linkedResult) return staleReply();
    if (!linkedResult && !option && context.recentSource && pointsAtRecentResult(normal)) linkedResult = &*context.recentSource;
    const bool pointsToResult = option != 0 || resultReference(normal) || linkedResult || (!named.empty() && readResult(normal));
    if (pointsToResult) {
        if (option < 0 || (!linkedResult && named.size() > 1)) return chooseTask(segment, named);
        if (!linkedResult) {
            if (preferred) linkedResult = resultForTask(context, preferred);
            else if (named.size() == 1) linkedResult = resultForTask(context, named.front()->id);
            else if (!context.rankedResults.empty()) linkedResult = &context.rankedResults.front();
            if (linkedResult && option > 0 && std::holds_alternative<MissingOption>(findOptionExcerpt(linkedResult->text, option))) {
                const auto alternative = std::ranges::find_if(context.rankedResults, [&](const Result& source) {
                    return source.taskId == linkedResult->taskId && source.revision == linkedResult->revision &&
                           !std::holds_alternative<MissingOption>(findOptionExcerpt(source.text, option));
                });
                if (alternative != context.rankedResults.end()) linkedResult = &*alternative;
            }
        }
        if (!linkedResult)
            return clarify(segment, translate(ja, "Which result should I look at?", "どの回答や作業結果を参照しますか？"));
        if (!question(normal) && explicitlySelectsImplementation(normal) && (option > 0 || linkedResult))
            return resultWork(segment, context, *linkedResult, option);
        if (readResult(normal) || question(normal)) return resultReply(segment, *linkedResult, option);
        if (doWorkOnResult(normal)) return resultWork(segment, context, *linkedResult, option);
        return clarify(segment, translate(ja, "What would you like me to do with this result?", "この結果について何をしますか？"));
    }

    if (!independent && explicitBudgetGrant(normal)) {
        const auto waiting = findTasks(context, [](const Task& task) { return task.waitId && task.waitReason == "budget"; });
        const auto candidates = targets(waiting, normal, preferred);
        if (candidates.size() == 1) return waitReply(segment, normal, *candidates.front());
        return chooseTask(segment, candidates);
    }
    if (mentionsAdditionalRuns(normal)) {
        return clarify(segment, translate(ja, "Use the current budget request to add one run.", "現在の予算確認に「追加1回」と返信してください。"));
    }
    const bool saysAnswer = starts(normal, {"質問への回答", "回答:", "回答：", "回答は", "answer:"});
    if (!independent && saysAnswer) {
        const auto waiting = findTasks(context, [](const Task& task) { return task.waitId && task.waitReason == "user_question"; });
        const auto candidates = targets(waiting, normal, preferred);
        if (candidates.size() == 1) return waitReply(segment, normal, *candidates.front());
        return chooseTask(segment, candidates);
    }
    if (genericAnswer(normal)) return clarify(segment, translate(ja, "Which question or task are you replying to?", "どの質問または作業への返信ですか？"));
    if (const auto answer = chatReply(normal, ja); !answer.empty()) return message(Kind::respond, segment, answer);
    if (!independent) {
        const auto waiting = findTasks(context, [&](const Task& task) { return offersAnswer(task, sentence(normal)); });
        const auto candidates = targets(waiting, normal, preferred);
        if (candidates.size() == 1) return waitReply(segment, normal, *candidates.front());
        if (candidates.size() > 1) return chooseTask(segment, candidates);
    }

    const auto environment = executionEnvironment(normal, context.environments);
    const bool criteria = criteriaChange(normal), scope = scopeChange(normal);
    const bool branch = any(normal, {"mainで", "main で", "main branch", "on main", "on the main", "ブランチに変更"});
    if (!independent && (!environment.alias.empty() || environment.ambiguous || criteria || scope || continuation(normal))) {
        const bool unresolved = namesOnlyClosedTasks || (!preferred && named.empty() && hasUnmatchedTarget(normal));
        if (unresolved || question(normal) || liveTargets.size() != 1) return chooseTask(segment, unresolved ? Candidates{} : liveTargets);
        if (environment.ambiguous)
            return clarify(segment, translate(ja, "Which execution environment should I use?", "どの実行環境を使いますか？"), liveTargets);
        const auto& task = *liveTargets.front();
        auto out = chosen((criteria || scope) ? Kind::reviseTask : Kind::appendInstruction, segment, &task);
        out.text = segment;
        out.environment = environment.alias;
        out.conflicting = criteria || scope || branch || !environment.alias.empty();
        if (criteria || scope) {
            if (scope) out.intent = fieldChange(segment, normal, false);
            if (criteria) out.doneWhen = fieldChange(segment, normal, true);
        }
        return out;
    }

    const bool incompleteReference = starts(normal, {"これ", "それ", "あれ", "この", "その", "あの", "it ", "that ", "this ", "the same"});
    const bool inquiry = starts(normal, {"what ", "why ", "how ", "is it ", "would it ", "when ", "where "}) ||
                         any(normal, {"とは？", "とは?", "とは何", "どういう意味", "どうやる", "どうすれば", "できますか", "するには"});
    if (!inquiry && requestWork(normal) && (independent || !incompleteReference)) return message(Kind::createTask, segment, std::string(segment));
    return clarify(segment, translate(ja, "Which task or earlier response should I use, or what new work should I start?",
                                       "どの作業や回答の話か、または新しく始めたい作業を教えてください。"));
}

struct Segment { std::string_view text; bool independent = false; };
std::vector<Segment> segments(std::string_view text) {
    const std::string owned(text);
    static const std::regex marker(R"((^|[\n.;!?][ \t]*|。\s*|、\s*|；\s*)(別件(?:として|で)?[ \t]*(?::|：|、)?[ \t]*|新規依頼(?::|：)[ \t]*|separately[ \t]*:[ \t]*|new task[ \t]*:[ \t]*))", std::regex::icase);
    std::vector<Segment> parts;
    std::size_t pos = 0;
    bool independent = false;
    for (std::sregex_iterator match(owned.begin(), owned.end(), marker), end; match != end; ++match) {
        const auto boundary = static_cast<std::size_t>(match->position());
        const auto before = trim(text.substr(pos, boundary - pos));
        if (!before.empty()) parts.push_back({before, independent});
        pos = boundary + static_cast<std::size_t>(match->length());
        independent = true;
    }
    const auto tail = trim(text.substr(pos));
    if (!tail.empty() || independent) parts.push_back({tail, independent});
    return parts;
}

std::string_view selectionLabel(std::string_view label) {
    for (auto suffix : {"の方です"sv, "の方"sv, "です"sv, "をお願いします"sv, " please"sv}) {
        if (label.ends_with(suffix)) label = trim(label.substr(0, label.size() - suffix.size()));
    }
    return label;
}

Id selectedCandidate(std::string_view segment, const Context& context) {
    const auto normal = normalize(sentence(segment));
    const auto label = selectionLabel(normal);
    int number = positiveInt(label);
    for (auto suffix : {"件目"sv, "番目"sv}) {
        if (!number && label.ends_with(suffix)) number = positiveInt(label.substr(0, label.size() - suffix.size()));
    }
    const auto& candidates = context.reply.candidates;
    if (number > 0 && number <= static_cast<int>(candidates.size())) return candidates[number - 1];
    Id selected = 0;
    for (const auto id : candidates) {
        const auto* task = taskById(context, id);
        if (!task) continue;
        if (!namesCandidate(segment,shortName(*task))) continue;
        if (selected) return 0;
        selected = id;
    }
    return selected;
}

}

bool namesCandidate(std::string_view reply, std::string_view taskName) {
    if (!trim(reply).empty() && trim(reply) == trim(taskName)) return true;
    const auto normalized = normalize(sentence(reply));
    const auto label = selectionLabel(normalized);
    const auto name = normalize(bounded(trim(taskName.substr(0,taskName.find('\n'))),160));
    return !label.empty() && (label == name || label == shortNamePrefix(name));
}

bool usesResults(std::string_view text) {
    const auto normal = normalize(text);
    return ordinal(normal) != 0 || resultReference(normal) || readResult(normal) || pointsAtRecentResult(normal);
}

std::vector<Route> route(std::string_view text, const Context& context) {
    const auto& reply = context.reply;
    if (reply.kind == Reply::Kind::invalid || (reply.kind == Reply::Kind::result && !reply.result))
        return {classify(text, context, false, 0, reply)};
    if (reply.kind == Reply::Kind::wait) {
        const auto* task = taskById(context, reply.taskId);
        if (!task || !active(*task) || !reply.waitId || task->waitId != reply.waitId)
            return {classify(text, context, false, 0, reply)};
    }
    Id target = 0;
    if (reply.kind == Reply::Kind::clarification) {
        const auto normal = normalize(sentence(text));
        const bool ja = japanese(text);
        if (oneOf(normal, {"この確認は不要", "この確認を取り下げます", "確認を取り消して", "この話は取り下げます", "forget it",
                           "dismiss this clarification", "never mind", "withdraw this request"})) {
            auto out = message(Kind::respond, text, translate(ja, "I’ve closed that clarification.", "この確認を閉じました。"));
            out.clarification = reply.id;
            out.superseded = true;
            return {std::move(out)};
        }
        target = selectedCandidate(text, context);
        if (!target || reply.originalText.empty()) {
            auto out = classify(text, context, false, 0, {});
            if (out.kind == Kind::status) return {std::move(out)};
            return {clarify(text, translate(ja, "Reply with the name of the task you mean.", "対象の作業名で返信してください。"))};
        }
        text = reply.originalText;
    }

    auto parts = segments(text);
    if (parts.empty()) return {clarify(text, translate(japanese(text), "What work should I start?", "どの作業を始めますか？"))};
    if (parts.size() > 16) return {clarify(text, translate(japanese(text), "Which work should I start with?", "どの作業から始めますか？"))};
    std::vector<Route> result;
    for (const auto& part : parts) {
        const auto& anchor = result.empty() && reply.kind != Reply::Kind::clarification ? reply : Reply{};
        auto out = classify(part.text, context, part.independent, target, anchor);
        if (target && out.kind != Kind::clarify) out.clarification = reply.id;
        result.push_back(std::move(out));
    }
    return result;
}
}
