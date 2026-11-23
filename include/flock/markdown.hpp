#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace flockv0::markdown {
using SensitiveText = std::function<bool(std::string_view)>;

struct SlackReply {
    std::string text;
    bool mrkdwn = true;
};

std::string toHtml(std::string_view source);
std::string toHtml(std::string_view source, const SensitiveText& sensitive);
SlackReply toSlack(std::string_view source);
std::string toPlain(std::string_view source);
}
