#include "conversation_router.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <regex>
#include <set>
#include <utility>
#include <variant>

namespace flockv0::conversation {
namespace {
using namespace std::literals;

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

int positiveInt(std::string_view text) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value > 0 ? value : 0;
}
int ordinal(std::string_view text) {
    static const std::regex number(R"((?:^|[^0-9])([1-9][0-9]?)(案目|番目|つ目)|\b(?:option|proposal|alternative|choice)\s*#?([1-9][0-9]?)(?![0-9])|案\s*([1-9][0-9]?)(?![0-9]|件))");
    int found = 0;
    auto add = [&](int value) {
        found = found != 0 && found != value ? -1 : value;
    };
    using Iterator = std::regex_iterator<std::string_view::const_iterator>;
    for (Iterator match(text.begin(), text.end(), number), end; match != end; ++match) {
        const auto& token = *match;
        const auto before = trim(text.substr(0,token.position()));
        if (trim(text.substr(token.position() + token.length())).starts_with("以外") ||
            before.ends_with("except") || before.ends_with("excluding")) return -1;
        add(positiveInt(token[1].matched ? token[1].str() : token[3].matched ? token[3].str() : token[4].str()));
    }
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
std::optional<std::string> findOptionExcerpt(std::string_view fullText, int wanted) {
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
                if (option.number == wanted) return std::nullopt;
                end = std::min(end, pos);
            }
        }
        if (newline == fullText.npos) break;
        pos = newline + 1;
    }
    if (start == fullText.npos) return std::nullopt;
    return bounded(trim(fullText.substr(start, end - start)), 8192);
}

const Task* taskById(const Context& context, Id id) {
    const auto task = std::ranges::find(context.tasks, id, &Task::id);
    return task == context.tasks.end() ? nullptr : &*task;
}

Target target(const Task& task) {
    return {task.id, task.generation, task.revision, task.waitId};
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

Status status(std::string_view text, const Context& context, const Task* selected = nullptr) {
    const bool ja = japanese(text);
    if (selected) return {statusLine(*selected, ja), target(*selected)};
    std::string response;
    std::size_t count = 0;
    for (const auto& task : context.tasks) {
        if (task.state == "completed" || task.state == "cancelled") continue;
        if (count == 10) break;
        if (count++) response += '\n';
        response += statusLine(task, ja);
    }
    if (response.empty() && !context.tasks.empty()) {
        const auto& task = context.tasks.front();
        return {statusLine(task, ja), target(task)};
    }
    if (response.empty())
        response = translate(ja, "There are no tasks for this Bot yet.", "この Bot にはまだ作業がありません。");
    else if (count == 10 || context.moreTasks)
        response += translate(ja, "\nOpen the task list for the full status.", "\nすべての進捗は作業一覧で確認できます。");
    return {std::move(response), std::nullopt};
}

template<class T>
Plan single(std::string_view segment, T action) {
    return {{{std::string(segment), std::move(action)}}, std::nullopt};
}

Plan unclear(std::string_view text, const std::set<Id>& candidates) {
    return single(text, Clarify{
        translate(japanese(text), "Please clarify which task, result, or question you mean and what you want me to do.",
                                  "対象の作業・結果・質問と、希望する対応を教えてください。"),
        {candidates.begin(), candidates.end()}
    });
}

bool genericAnswer(std::string_view text) {
    const auto normal = normalize(text);
    return oneOf(sentence(normal), {"はい", "はい、お願いします", "はい、お願い", "いいえ", "うん", "そう",
        "ok", "okay", "yes", "no", "sure", "yes please", "yes, please", "了解", "了解です", "それで", "そのまま",
        "this one", "that one"});
}

std::optional<std::string_view> cancellationSubject(std::string_view text) {
    auto command = sentence(text);
    for (auto prefix : {"please "sv,"can you "sv,"could you "sv,"would you "sv,"i want you to "sv}) {
        if (command.starts_with(prefix)) {
            command.remove_prefix(prefix.size());
            break;
        }
    }
    for (auto verb : {"stop"sv,"cancel"sv,"abort"sv,"halt"sv}) {
        if (command == verb) return ""sv;
        if (command.starts_with(verb) && command[verb.size()] == ' ') {
            auto subject = trim(command.substr(verb.size()));
            if (subject.ends_with(" please")) subject = trim(subject.substr(0,subject.size() - 7));
            return subject;
        }
    }
    for (auto suffix : {"ください"sv,"ほしい"sv,"もらえますか"sv,"くれますか"sv}) {
        if (command.ends_with(suffix)) {
            command = trim(command.substr(0,command.size() - suffix.size()));
            break;
        }
    }
    for (auto verb : {"止めて"sv,"停止して"sv,"中止して"sv,"キャンセルして"sv,"取り消して"sv,"やめて"sv}) {
        if (command.ends_with(verb)) return trim(command.substr(0,command.size() - verb.size()));
    }
    if (oneOf(command,{"中止","停止","キャンセル"})) return ""sv;
    return std::nullopt;
}

bool groundedCancellation(std::string_view text, const Task& task, const Context& context,
                          Id selected) {
    const auto normal = normalize(text);
    const auto subject = cancellationSubject(normal);
    if (!subject) return false;
    const bool wholeTask = oneOf(*subject,{"","it","that","this","the task","this task","that task","the current task",
            "それ","それを","これ","これを","この作業を","その作業を","今の作業を","作業を","一旦","いったん"});
    auto name = *subject;
    if (name.starts_with("the task ")) name.remove_prefix(9);
    if (name.ends_with("を")) name.remove_suffix("を"sv.size());
    for (const auto& [left,right] : {std::pair{"「"sv,"」"sv},std::pair{"\""sv,"\""sv}}) {
        if (name.size() >= left.size() + right.size() && name.starts_with(left) && name.ends_with(right))
            name = name.substr(left.size(),name.size()-left.size()-right.size());
    }
    const auto matches = [&](const Task& candidate) {
        return active(candidate) && name == normalize(trim(candidate.fullName));
    };
    if (selected) return task.id == selected && (wholeTask || matches(task));
    if (matches(task) && std::ranges::count_if(context.tasks,matches) == 1) return true;
    return wholeTask && !context.moreTasks && std::ranges::count_if(context.tasks,active) == 1;
}

bool dismissesClarification(std::string_view text) {
    const auto normal = normalize(text);
    return oneOf(sentence(normal), {"この確認は不要", "この確認を取り下げます", "確認を取り消して", "この話は取り下げます",
        "forget it", "dismiss this clarification", "never mind", "withdraw this request"});
}

std::string_view selectionLabel(std::string_view label) {
    for (auto suffix : {"の方です"sv, "の方"sv, "です"sv, "をお願いします"sv, " please"sv}) {
        if (label.ends_with(suffix)) return trim(label.substr(0, label.size() - suffix.size()));
    }
    return label;
}

Id namedClarificationCandidate(std::string_view text, const Context& context, const std::vector<Id>& candidates) {
    const auto normal = normalize(sentence(text));
    const auto label = selectionLabel(normal);
    auto number = positiveInt(label);
    for (auto suffix : {"件目"sv, "番目"sv}) {
        if (!number && label.ends_with(suffix)) number = positiveInt(label.substr(0, label.size() - suffix.size()));
    }
    if (label.starts_with('#')) {
        const auto remainder = label.substr(1);
        Id id = 0;
        const auto parsed = std::from_chars(remainder.data(),remainder.data() + remainder.size(),id);
        if (parsed.ec == std::errc{} && parsed.ptr == remainder.data() + remainder.size() &&
            std::ranges::find(candidates,id) != candidates.end()) return id;
        return 0;
    }
    if (number > 0 && static_cast<std::size_t>(number) <= candidates.size()) return candidates[number - 1];
    Id selected = 0;
    for (const auto id : candidates) {
        const auto* task = taskById(context, id);
        if (!task || !namesCandidate(text, task->fullName)) continue;
        if (selected) return 0;
        selected = id;
    }
    return selected;
}

struct InvalidRoute {};

void require(bool valid) {
    if (!valid) throw InvalidRoute{};
}

void checkFields(const Json& object, std::initializer_list<std::string_view> fields) {
    require(object.is_object());
    for (auto it = object.begin(); it != object.end(); ++it)
        require(oneOf(it.key(), fields));
}

std::string readText(const Json& object, std::string_view key, std::size_t limit) {
    const auto value = object.find(key);
    if (value == object.end()) return {};
    require(value->is_string());
    const auto& text = value->get_ref<const std::string&>();
    require(!trim(text).empty() && text.size() <= limit);
    return text;
}

Id readId(const Json& value) {
    require(value.is_number_integer());
    if (value.is_number_unsigned())
        require(value.get<std::uint64_t>() <= static_cast<std::uint64_t>(std::numeric_limits<Id>::max()));
    const auto id = value.get<Id>();
    require(id > 0);
    return id;
}

Id readId(const Json& object, std::string_view key) {
    const auto value = object.find(key);
    return value == object.end() ? 0 : readId(*value);
}

bool delimitersOnly(std::string_view text) {
    constexpr std::array delimiters{"。"sv, "、"sv, "；"sv, "！"sv, "？"sv, "　"sv};
    while (!text.empty()) {
        if (" \t\n\r.,;!?"sv.find(text.front()) != std::string_view::npos) {
            text.remove_prefix(1);
            continue;
        }
        const auto prefix = std::ranges::find_if(delimiters, [&](auto d) { return text.starts_with(d); });
        if (prefix == delimiters.end()) return false;
        text.remove_prefix(prefix->size());
    }
    return true;
}

bool segmentBoundary(std::string_view text) {
    if (delimitersOnly(text)) return true;
    for (auto marker : {"別件として"sv,"別件で"sv,"別件"sv,"新規依頼:"sv,"新規依頼："sv,"separately:"sv,"new task:"sv}) {
        const auto position = text.find(marker);
        if (position != text.npos && delimitersOnly(text.substr(0, position)) &&
            delimitersOnly(text.substr(position + marker.size()))) return true;
    }
    return false;
}

std::vector<std::string> segments(const Json& routes, std::string_view text) {
    require(routes.is_array() && !routes.empty() && routes.size() <= 8);
    std::vector<std::string> result;
    for (const auto& route : routes) {
        require(route.is_object());
        auto part = readText(route, "segment", 16000);
        if (routes.size() == 1) {
            require(part.empty() || part == text);
            result.emplace_back(text);
            return result;
        }
        require(!part.empty());
        const auto at = text.find(part);
        require(at != text.npos && segmentBoundary(text.substr(0, at)));
        text.remove_prefix(at + part.size());
        result.push_back(std::move(part));
    }
    require(delimitersOnly(text));
    return result;
}

Json resultData(const Result& result, std::size_t limit) {
    return {{"taskId",result.taskId},{"messageId",result.messageId},{"request",result.taskName},
            {"state",result.state},{"text",bounded(result.text, limit)}};
}

Json clarificationData(const Reply& reply) {
    Json waits = Json::array();
    for (const auto& [taskId,waitId] : reply.waitsAtIntake)
        waits.push_back({{"taskId",taskId},{"waitId",waitId}});
    Json data = {{"messageId",reply.id},{"originalText",reply.originalText},{"question",reply.question},
                 {"candidates",reply.candidates},{"operation",reply.operation},{"waitsAtIntake",std::move(waits)},
                 {"incompleteWaitsAtIntake",reply.incompleteWaitsAtIntake}};
    if (reply.result) data["result"] = resultData(*reply.result,1024);
    if (reply.revisionDraft) {
        const auto& draft = *reply.revisionDraft;
        data["revisionDraft"] = {{"taskId",draft.target.taskId},{"generation",draft.target.generation},
                                 {"revisionId",draft.target.revision},{"waitId",draft.target.waitId},
                                 {"intent",draft.intent},{"doneWhen",draft.doneWhen}};
    }
    return data;
}

Json decisionContext(std::string_view text, const Context& context) {
    Json tasks = Json::array(), results = Json::array();
    for (const auto& task : context.tasks) {
        Json wait = nullptr;
        if (task.waitId) {
            wait = {{"waitId",task.waitId},{"reason",task.waitReason},{"text",task.waitText},
                    {"choices",task.waitChoices},{"currentAtIntake",task.waitAtIntake}};
        }
        tasks.push_back({{"taskId",task.id},{"request",task.name},{"intent",task.intent},{"state",task.state},
                        {"doneWhen",task.doneWhen},{"wait",wait},{"environmentId",task.environmentId},
                        {"projectId",task.projectId}});
    }
    for (const auto& result : context.rankedResults) results.push_back(resultData(result, 512));
    const auto& reply = context.reply;
    const auto kind = [&] {
        switch (reply.kind) {
            case Reply::Kind::none: return "none";
            case Reply::Kind::invalid: return "invalid";
            case Reply::Kind::wait: return "wait";
            case Reply::Kind::result: return "result";
            case Reply::Kind::clarification: return "clarification";
        }
        throw std::logic_error("Unknown conversation reply");
    }();
    Json anchor = {{"kind",kind},{"messageId",reply.id},{"taskId",reply.taskId},{"waitId",reply.waitId},
                   {"originalText",reply.originalText},{"candidates",reply.candidates}};
    if (reply.kind == Reply::Kind::clarification) anchor.update(clarificationData(reply));
    if (reply.result) {
        anchor["resultMessageId"] = reply.result->messageId;
        anchor["result"] = resultData(*reply.result, 1024);
    }
    return {{"kind","route_conversation"},{"text",text},{"tasks",tasks},{"results",results},{"reply",anchor},
            {"pendingClarification",context.pendingClarification ? clarificationData(*context.pendingClarification) : Json(nullptr)},
            {"multiplePendingClarifications",context.multiplePendingClarifications},
            {"recentSource",context.recentSource ? resultData(*context.recentSource, 1024) : Json(nullptr)},
            {"moreTasks",context.moreTasks},{"moreWaits",context.moreWaits},{"staleWaits",context.staleWaits},
            {"expiredWaits",context.expiredWaits},{"environments",context.environments}};
}

bool refersToResult(std::string_view text) {
    return starts(trim(text), {"それを","それで","これを","その案","implement it","implement that","implement this",
                               "use that","use this","do that"});
}

std::optional<Result> selectResult(Id id, int numbered, const Context& context, const Reply& anchor,
                                   std::string_view segment) {
    const auto normal = normalize(segment);
    if (anchor.result && anchor.kind == Reply::Kind::clarification) {
        require(!id || id == anchor.result->messageId);
        require(!numbered || numbered == anchor.result->option || anchor.result->option == 0);
        return anchor.result;
    }
    const bool needsSource = numbered || refersToResult(normal);
    const Result* source = nullptr;
    if (anchor.result) {
        require(!id || anchor.result->messageId == id);
        source = &*anchor.result;
    } else if (context.recentSource && needsSource &&
               !std::ranges::any_of(context.tasks,[&](const Task& t) { return refersTo(normal,normalize(t.fullName)); })) {
        require(!id || context.recentSource->messageId == id);
        source = &*context.recentSource;
    } else if (id) {
        const auto match = std::ranges::find(context.rankedResults,id,&Result::messageId);
        require(match != context.rankedResults.end());
        source = &*match;
        if (needsSource) {
            std::set<Id> named, available;
            for (const auto& entry : context.rankedResults) {
                available.insert(entry.taskId);
                if (refersTo(normal,normalize(entry.taskName))) named.insert(entry.taskId);
            }
            const auto& plausible = named.empty() ? available : named;
            require(plausible.size() == 1 && plausible.contains(source->taskId));
        }
    }
    require(source || !needsSource);
    if (!source) return std::nullopt;
    auto result = *source;
    if (!numbered || result.option == numbered) return result;
    if (result.option) {
        const auto full = std::ranges::find(context.rankedResults,result.messageId,&Result::messageId);
        require(full != context.rankedResults.end());
        result = *full;
    }
    auto excerpt = findOptionExcerpt(result.text,numbered);
    require(excerpt.has_value());
    result.text = std::move(*excerpt);
    result.option = numbered;
    return result;
}

std::set<std::string> mentionedDestinations(const Context& context, std::string_view text) {
    const auto normal = normalize(text);
    std::set<std::string> destinations;
    for (auto entry = context.destinationIds.begin(); entry != context.destinationIds.end(); ++entry)
        if (refersTo(normal,normalize(entry.key()))) destinations.insert(entry.value().get<std::string>());
    return destinations;
}

bool destinationMatches(const Context& context, std::string_view chosen, const std::set<std::string>& mentioned) {
    const auto allowed = context.destinationIds.find(chosen);
    return allowed != context.destinationIds.end() &&
        (mentioned.empty() || (mentioned.size() == 1 && mentioned.contains(allowed->get<std::string>())));
}

bool groundedWait(const Task& task, const Context& context, const Reply& anchor, std::string_view segment) {
    if (anchor.kind == Reply::Kind::wait)
        return anchor.taskId == task.id && anchor.waitId == task.waitId;
    if (!task.waitAtIntake || context.staleWaits || context.moreWaits || genericAnswer(segment)) return false;
    if (!namedTargetMatches(segment,context,task.id)) return false;
    const auto matches = [&](const Task& candidate) {
        return candidate.waitAtIntake && candidate.waitReason == task.waitReason;
    };
    if (std::ranges::count_if(context.tasks, matches) == 1) return true;
    const auto normal = normalize(segment);
    const auto identifies = [&](const Task& candidate) {
        return matches(candidate) && namesTask(normal, candidate);
    };
    if (identifies(task) && std::ranges::count_if(context.tasks, identifies) == 1) return true;
    const auto answer = sentence(normal);
    const auto offers = [&](const Task& candidate) {
        return matches(candidate) && std::ranges::any_of(candidate.waitChoices, [&](const auto& choice) {
            return answer == normalize(trim(choice));
        });
    };
    return offers(task) && std::ranges::count_if(context.tasks, offers) == 1;
}

Action parseAction(const Json& route, std::string_view segment, const Context& context, const Reply& anchor,
                   Id selected, std::string_view clarification, const Reply* resolved) {
    const auto kind = readText(route, "kind", 32);
    if (anchor.kind == Reply::Kind::clarification && anchor.result)
        require((kind == "answer_result" || kind == "clarify") && resolved);
    const auto id = readId(route, "taskId");
    const auto* task = taskById(context, id);
    const auto resultId = readId(route, "resultMessageId");
    const auto proposedOption = readId(route, "resultOption");
    const auto mentionedOption = (resultId || anchor.result || proposedOption ||
        kind == "create_task" || kind == "append_instruction" || kind == "revise_task") ? ordinal(normalize(segment)) : 0;
    const auto selectedOption = kind == "answer_result" && !proposedOption && mentionedOption < 0
        ? 0 : mentionedOption;
    require(proposedOption <= 99 && selectedOption >= 0 &&
            (!selectedOption || !proposedOption || selectedOption == proposedOption));
    const auto option = static_cast<int>(proposedOption ? proposedOption : selectedOption);
    require(!id || task);
    if (kind == "clarify") {
        checkFields(route, {"kind","segment","text","candidates","operation","resultMessageId"});
        auto question = readText(route, "text", 2000);
        require(!question.empty());
        std::vector<Id> choices;
        if (const auto list = route.find("candidates"); list != route.end()) {
            require(list->is_array() && list->size() <= context.tasks.size());
            for (const auto& value : *list) {
                const auto candidate = readId(value);
                const auto* found = taskById(context, candidate);
                require(found && std::ranges::find(choices,candidate) == choices.end());
                choices.push_back(candidate);
            }
        }
        auto operation = readText(route,"operation",32);
        require(operation.empty() || oneOf(operation,{"cancel_task","revise_task","destination"}));
        if (operation == "destination") require(choices.size() <= 1);
        if (operation == "cancel_task" && !explicitCancellation(segment)) operation.clear();
        auto source = resultId || anchor.result ? selectResult(resultId,option,context,anchor,segment) : std::nullopt;
        return Clarify{std::move(question),std::move(choices),std::move(source),std::move(operation)};
    }
    if (kind == "status") {
        checkFields(route, {"kind","segment","taskId"});
        return status(segment, context, task);
    }
    if (kind == "respond") {
        checkFields(route, {"kind","segment","text"});
        auto text = readText(route, "text", 4000);
        require(!text.empty() && anchor.kind != Reply::Kind::result);
        return Respond{std::move(text), std::nullopt};
    }
    if (kind == "answer_result") {
        checkFields(route, {"kind","segment","resultMessageId","resultOption"});
        auto source = selectResult(resultId,option,context,anchor,segment);
        require(source && !source->text.empty() && anchor.kind != Reply::Kind::wait);
        return Respond{{}, std::move(source)};
    }
    if (kind == "create_task") {
        checkFields(route, {"kind","segment","resultMessageId","resultOption","environment"});
        require(anchor.kind != Reply::Kind::wait && !genericAnswer(segment) && !explicitBudgetGrant(segment));
        auto source = selectResult(resultId,option,context,anchor,segment);
        const auto mentioned = mentionedDestinations(context,segment);
        auto environment = readText(route,"environment",256);
        if (!mentioned.empty() && (environment.empty() || !destinationMatches(context,environment,mentioned)))
            return Clarify{translate(japanese(segment),"Which configured execution destination should I use?",
                                      "設定済みの実行先を選んでください。"),{},std::move(source),"destination"};
        require(environment.empty() || destinationMatches(context,environment,mentioned));
        require(!source || oneOf(source->state, {"completed","cancelled"}));
        const auto instruction = normalize(segment);
        const auto correction = normalize(clarification);
        const auto namesExistingWork = [&](const Task& existing) {
            const auto request = normalize(existing.name);
            return active(existing) && existing.waitReason == "budget" &&
                   (sentence(request) == sentence(instruction) || sentence(request) == sentence(correction));
        };
        require(!std::ranges::any_of(context.tasks, namesExistingWork));
        return CreateTask{std::string(segment),std::move(source),std::move(environment)};
    }

    require(task && active(*task));
    require(anchor.kind != Reply::Kind::wait || anchor.taskId == task->id);
    if (kind == "answer_wait") {
        checkFields(route, {"kind","segment","taskId"});
        require(task->waitId && anchor.kind != Reply::Kind::result);
        if (resolved) {
            require(selected == task->id && task->waitAtIntake && !resolved->incompleteWaitsAtIntake &&
                    std::ranges::find(resolved->waitsAtIntake,std::pair{task->id,task->waitId}) != resolved->waitsAtIntake.end());
        } else require(groundedWait(*task, context, anchor, segment));
        require(task->waitReason == "user_question" && segment.size() <= 8000);
        return AnswerWait{target(*task),std::string(segment)};
    }
    if (kind == "cancel_task") {
        checkFields(route, {"kind","segment","taskId"});
        require(anchor.kind != Reply::Kind::result && groundedCancellation(segment,*task,context,selected));
        return CancelTask{target(*task)};
    }

    require(namedTargetMatches(segment,context,task->id));
    auto source = selectResult(resultId,option,context,anchor,segment);
    require(!source || (source->taskId == task->id && source->revision == task->revision));
    if (kind == "append_instruction") {
        checkFields(route, {"kind","segment","taskId","resultMessageId","resultOption"});
        return AppendInstruction{target(*task), std::string(segment), std::move(source)};
    }
    require(kind == "revise_task");
    checkFields(route, {"kind","segment","taskId","resultMessageId","resultOption","intent","doneWhen","environment"});
    auto intent = readText(route, "intent", 4000);
    auto done = readText(route, "doneWhen", 4000);
    auto environment = readText(route, "environment", 256);
    require(!intent.empty() || !done.empty() || !environment.empty());
    const auto verbatim = [&](std::string_view value) {
        return value.empty() || segment.find(value) != segment.npos || clarification.find(value) != clarification.npos;
    };
    require(verbatim(intent) && verbatim(done));
    auto destinations = mentionedDestinations(context,segment);
    const auto replyDestinations = mentionedDestinations(context,clarification);
    if (!replyDestinations.empty()) destinations = replyDestinations;
    if (!destinations.empty() && (environment.empty() || !destinationMatches(context,environment,destinations)))
        return Clarify{translate(japanese(segment),"Which configured execution destination should I use?",
                                  "設定済みの実行先を選んでください。"),{task->id},std::move(source),"destination",
                       RevisionDraft{target(*task),std::move(intent),std::move(done)}};
    require(environment.empty() || (destinationMatches(context,environment,destinations) ||
        (environment == task->environmentId && verbatim(environment))));
    return ReviseTask{target(*task), std::string(segment), std::move(intent), std::move(done),
                      std::move(environment), std::move(source)};
}

Id actionTask(const Action& action) {
    return std::visit([](const auto& value) -> Id {
        if constexpr (requires { value.target.taskId; }) return value.target.taskId;
        else if constexpr (requires { value.target; }) return value.target ? value.target->taskId : 0;
        else if constexpr (requires { value.source; }) return value.source ? value.source->taskId : 0;
        else return 0;
    }, action);
}

Plan parsePlan(const Json& args, std::string_view text, const Context& context, std::set<Id>& targets) {
    checkFields(args, {"routes","resolution","clarificationMessageId"});
    require(args.contains("routes"));
    const auto resolution = readText(args, "resolution", 16);
    const auto clarificationId = readId(args, "clarificationMessageId");
    require(!clarificationId || !resolution.empty());
    Plan plan;
    Id selected = 0;
    const Reply* resolved = nullptr;
    std::string_view clarificationReply;
    if (!resolution.empty()) {
        const auto* pending = context.clarification(clarificationId);
        require(pending);
        if (resolution == "superseded") {
            require(context.reply.kind == Reply::Kind::clarification && dismissesClarification(text));
            plan.resolution = Resolution{pending->id, Resolution::Kind::superseded};
        } else {
            require(resolution == "resolved" && !trim(pending->originalText).empty());
            resolved = pending;
            plan.resolution = Resolution{pending->id, Resolution::Kind::resolved};
            selected = namedClarificationCandidate(text, context, pending->candidates);
            require(selected || !genericAnswer(text) || (pending->result && context.reply.id == pending->id));
            clarificationReply = text;
            text = pending->originalText;
        }
    }
    const auto parts = segments(args.at("routes"), text);
    require(!resolved || parts.size() == 1);
    bool selectedCandidate = !resolved || resolved->candidates.empty();
    bool invalid = false;
    std::optional<Clarify> clarification;
    const Reply noReply;
    const auto& firstAnchor = resolved ? *resolved : context.reply;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto& anchor = index == 0 ? firstAnchor : noReply;
        try {
            const auto correction = !selected ? clarificationReply : std::string_view{};
            auto action = parseAction(args.at("routes").at(index),parts[index],context,anchor,selected,correction,resolved);
            if (auto* question = std::get_if<Clarify>(&action)) {
                targets.insert(question->candidates.begin(),question->candidates.end());
                if (!clarification) clarification = std::move(*question);
                continue;
            }
            const auto taskId = actionTask(action);
            if (resolved) {
                require(!taskId || resolved->candidates.empty() ||
                        std::ranges::find(resolved->candidates, taskId) != resolved->candidates.end());
                selectedCandidate = selectedCandidate || (taskId != 0 && (!selected || taskId == selected));
            }
            if (parts.size() > 1 && (std::holds_alternative<AppendInstruction>(action) ||
                std::holds_alternative<ReviseTask>(action) || std::holds_alternative<CancelTask>(action)))
                targets.insert(taskId);
            auto segment = parts[index];
            if (!correction.empty()) {
                auto withReply = segment + "\n\nUser clarification:\n" + std::string(correction);
                require(withReply.size() <= 16000);
                if (auto* creation = std::get_if<CreateTask>(&action)) creation->request = withReply;
                if (auto* append = std::get_if<AppendInstruction>(&action)) append->instruction = withReply;
                if (auto* revise = std::get_if<ReviseTask>(&action)) revise->instruction = withReply;
                if (const auto* answer = std::get_if<Respond>(&action); answer && answer->source)
                    segment += "\n\nClarification question:\n" + resolved->question + "\n\nUser clarification:\n" + std::string(correction);
            }
            plan.routes.push_back({std::move(segment),std::move(action)});
        } catch (const InvalidRoute&) {
            invalid = true;
        }
    }
    if (clarification) {
        clarification->candidates.assign(targets.begin(),targets.end());
        auto result = single(text,std::move(*clarification));
        if (resolved && resolved->result && std::get<Clarify>(result.routes.front().action).source)
            result.resolution = plan.resolution;
        return result;
    }
    require(!invalid);
    require(selectedCandidate);
    if (plan.resolution && plan.resolution->kind == Resolution::Kind::superseded)
        require(plan.routes.size() == 1 && std::holds_alternative<Respond>(plan.routes.front().action));
    return plan;
}

std::string quoteText(std::string_view text) {
    std::string result;
    const auto normal = normalize(text);
    for (const char c : normal)
        if (" \t\n\r-.>*_`"sv.find(c) == std::string_view::npos) result += c;
    return result;
}

Action answerResult(Model& model, std::string_view question, Result source, const BotNotes& notes, const Json& config) {
    const auto decision = model.decide({
        .task = Json::object(),
        .event = {{"kind","answer_conversation_result"},{"text",question},{"source",resultData(source, 8192)}},
        .history = Json::array(), .notes = notes, .tools = {"answer_conversation_result","clarify_conversation_result"},
        .feedback = {}, .config = config
    });
    checkFields(decision.args, {"text"});
    if (decision.tool == "clarify_conversation_result") {
        auto question = readText(decision.args,"text",2000);
        require(!question.empty());
        return Clarify{std::move(question),{},std::move(source)};
    }
    require(decision.tool == "answer_conversation_result");
    auto answer = readText(decision.args, "text", 4000);
    require(!answer.empty());
    if (source.text.size() >= 120 || source.text.find('\n') != std::string::npos)
        require(quoteText(answer).find(quoteText(source.text)) == std::string::npos);
    return Respond{std::move(answer),std::move(source)};
}
}

const Reply* Context::clarification(Id replyId) const {
    if (reply.kind == Reply::Kind::clarification)
        return !replyId || replyId == reply.id ? &reply : nullptr;
    if (pendingClarification && replyId == pendingClarification->id) return &*pendingClarification;
    return nullptr;
}

bool namesCandidate(std::string_view reply, std::string_view taskName) {
    const auto normal = normalize(sentence(reply));
    const auto label = selectionLabel(normal);
    const auto name = normalize(trim(taskName.substr(0,taskName.find('\n'))));
    return !label.empty() && (label == name || label == shortNamePrefix(name));
}

bool explicitBudgetGrant(std::string_view text) {
    const auto normal = normalize(text);
    return oneOf(sentence(normal), {"追加1回", "追加 1 回", "追加1回お願いします", "追加1回を許可", "追加1回を許可します",
        "あと1回追加", "あと1回追加して", "one more run", "one additional run", "add one more run",
        "grant one more run", "add one run", "approve one additional run", "allow one additional run"});
}

bool explicitCancellation(std::string_view text) {
    return cancellationSubject(normalize(text)).has_value();
}

bool namedTargetMatches(std::string_view text, const Context& context, Id targetId) {
    const auto normal = normalize(text);
    Id named = 0;
    for (const auto& task : context.tasks) {
        if (!refersTo(normal,normalize(trim(task.fullName.substr(0,task.fullName.find('\n')))))) continue;
        if (named && named != task.id) return false;
        named = task.id;
    }
    return !named || named == targetId;
}

std::optional<Plan> shortcut(std::string_view text, const Context& context) {
    const auto& reply = context.reply;
    const auto* task = taskById(context, reply.taskId);
    const bool ja = japanese(text);
    if (reply.kind == Reply::Kind::wait) {
        if (task && active(*task) && reply.waitId && task->waitId == reply.waitId) {
            if (task->waitReason == "user_question" && text.size() <= 8000)
                return single(text,AnswerWait{target(*task),std::string(text)});
            return single(text,Respond{translate(ja,"Use the available action on this waiting item.",
                                                   "確認待ちの項目から対応する操作を選んでください。"),std::nullopt});
        }
        return single(text, Respond{translate(ja, "That question is no longer current. Check the task for its latest status.",
                                                 "その質問の受付は終了しました。作業の最新状況を確認してください。"), std::nullopt});
    }
    if (reply.kind == Reply::Kind::invalid || (reply.kind == Reply::Kind::result && !reply.result)) {
        return single(text, Respond{translate(ja, "That reply target is unavailable in this Bot’s conversation.",
                                                 "この Bot の会話では返信先を確認できません。"), std::nullopt});
    }
    if (reply.kind == Reply::Kind::clarification && dismissesClarification(text)) {
        auto plan = single(text, Respond{translate(ja, "I closed that clarification.", "この確認を閉じました。"), std::nullopt});
        plan.resolution = Resolution{reply.id, Resolution::Kind::superseded};
        return plan;
    }
    if (reply.kind == Reply::Kind::clarification && reply.operation == "destination") {
        const auto destination = trim(text);
        const auto defer = [&](std::string_view en,std::string_view jp) {
            return single(text,Respond{translate(ja,en,jp),std::nullopt});
        };
        const auto supersede = [&](std::string_view en,std::string_view jp) {
            auto plan = defer(en,jp);
            plan.resolution = Resolution{reply.id,Resolution::Kind::superseded};
            return plan;
        };
        if (std::ranges::find(context.environments,destination) == context.environments.end())
            return defer("Choose a configured destination in the original confirmation.",
                         "元の確認から設定済みの実行先を選んでください。");
        auto instruction = reply.originalText + "\n\nExecution destination: " + std::string(destination);
        Plan plan;
        if (reply.candidates.empty())
            plan = single(reply.originalText,CreateTask{std::move(instruction),reply.result,std::string(destination)});
        else {
            const auto* selected = taskById(context,reply.candidates.front());
            if (reply.candidates.size() != 1 || !selected || !active(*selected)) {
                return supersede("The task for this destination is no longer active.",
                                 "実行先を変更する作業は終了しました。");
            }
            if (!reply.revisionDraft)
                return supersede("The task revision details are no longer available. Submit the revision again.",
                                 "変更内容を復元できません。作業の変更をもう一度送ってください。");
            const auto& draft = *reply.revisionDraft;
            if (draft.target.taskId != selected->id || draft.target.generation != selected->generation ||
                draft.target.revision != selected->revision || draft.target.waitId != selected->waitId)
                return supersede("The task changed while its destination was pending. Submit the revision again.",
                                 "実行先の確認中に作業が更新されました。変更をもう一度送ってください。");
            plan = single(reply.originalText,ReviseTask{draft.target,std::move(instruction),draft.intent,draft.doneWhen,
                                                        std::string(destination),reply.result});
        }
        plan.resolution = Resolution{reply.id,Resolution::Kind::resolved};
        return plan;
    }
    const auto normal = normalize(text);
    if (reply.kind == Reply::Kind::none &&
        oneOf(sentence(normal), {"進捗", "進捗は", "進捗はどう", "進捗状況", "進捗どうなってる",
                                 "status", "progress", "status update", "how is it going", "what's the status"}))
        return single(text, status(text, context));
    return std::nullopt;
}

Plan decide(std::string_view text, const Context& context, Model& model, const BotNotes& notes, const Json& config) {
    const auto decision = model.decide({
        .task = Json::object(), .event = decisionContext(text, context), .history = context.history,
        .notes = notes, .tools = {"route_conversation"}, .feedback = {}, .config = config
    });
    std::set<Id> targets;
    try {
        require(decision.tool == "route_conversation" && decision.args.is_object());
        auto plan = parsePlan(decision.args, text, context, targets);
        for (auto& route : plan.routes) {
            auto* response = std::get_if<Respond>(&route.action);
            if (!response || !response->source) continue;
            auto answer = answerResult(model, route.segment, std::move(*response->source), notes, config);
            if (std::holds_alternative<Clarify>(answer) && plan.routes.size() != 1)
                return unclear(text,targets);
            route.action = std::move(answer);
        }
        return plan;
    } catch (const InvalidRoute&) {
        if (context.reply.kind == Reply::Kind::clarification && context.reply.result)
            return single(text,Respond{translate(japanese(text),"Please answer the current clarification.",
                                                   "元の確認に回答してください。"),std::nullopt});
        auto plan = unclear(text,targets);
        auto& clarification = std::get<Clarify>(plan.routes.front().action);
        if (context.reply.result) clarification.source = context.reply.result;
        return plan;
    }
}
}
