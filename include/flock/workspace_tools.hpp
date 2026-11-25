#pragma once

#include "flock/core.hpp"

#include <map>

namespace flockv0 {

inline constexpr std::size_t maxToolCalls = 8;

const std::vector<std::string>& botToolNames();
bool isBotTool(std::string_view);
Json botToolSchema(std::string_view);

class WebTools {
public:
    virtual ~WebTools() = default;
    virtual Json search(std::string_view query) = 0;
    virtual Json fetch(std::string_view url) = 0;
};

struct ToolOutcomeUnknown : Conflict {
    using Conflict::Conflict;
};

enum class McpCollection { tools, resources };

struct McpToolResult {
    Json data;
    bool isError;
};

class McpTools {
public:
    virtual ~McpTools() = default;
    virtual bool hasServer(std::string_view server) const = 0;
    virtual bool readOnlyHint(std::string_view, std::string_view) { return false; }
    virtual Json discover(std::string_view bot, std::string_view server, McpCollection, std::string_view cursor) = 0;
    virtual McpToolResult invoke(std::string_view server, std::string_view tool, const Json& args) = 0;
    virtual Json readResource(std::string_view server, std::string_view uri) = 0;
};

struct BotTools {
    std::map<std::string, std::filesystem::path, std::less<>> skills;
    std::shared_ptr<WebTools> web;
    std::shared_ptr<McpTools> mcp;
};

}
