#pragma once

#include "flock/policy.hpp"

namespace flockv0 {

struct CompletionEntry;

struct ExternalMcpCredential {
    std::string connectionId, bearerToken;
};

struct ExternalMcpConnection {
    std::string id;
};

class ExternalMcp {
public:
    explicit ExternalMcp(Store& store) : store_(store) {}

    // Provisioned by the local installation operator. The bearer is returned only at creation.
    ExternalMcpCredential connect(std::string_view person, const std::vector<std::string>& bots);
    void disconnect(std::string_view connectionId);
    std::optional<ExternalMcpConnection> authenticate(std::string_view bearer) const;

    Id submit(const ExternalMcpConnection&, std::string_view bot, std::string_view requestId, std::string_view text, const IntakeResolver&);
    Json status(const ExternalMcpConnection&, Id taskId) const;
    void answer(const ExternalMcpConnection&, Id taskId, Id waitId, Id revision, Id generation, std::string_view text);
private:
    struct OwnedTask {
        Person actor;
        std::string bot;
    };
    OwnedTask authorizeTask(const ExternalMcpConnection&, Id taskId) const;
    Json completionRun(const OwnedTask&, const CompletionEntry&) const;
    Store& store_;
};

}
