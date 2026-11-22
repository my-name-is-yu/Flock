#include "storage.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flockv0 {
namespace {
std::filesystem::path localPath(const std::string& raw) {
    const std::filesystem::path path(raw);
    if (raw.size() > 4096 || raw.find('\0') != raw.npos || !path.is_absolute() ||
        raw != path.lexically_normal().string() || (path.has_relative_path() && path.filename().empty()))
        throw Conflict("Local operations need a normalized absolute path");
    return path;
}

std::string_view localOperation(std::string_view operation) {
    if (operation != "read_file" && operation != "list_directory")
        throw Conflict("Unsupported local operation");
    return operation;
}

Json localRules(const Json& args) {
    const auto rules = args.value("rules",Json::array());
    if (!rules.is_array() || rules.empty() || rules.size() > 20) throw Conflict("Request one to twenty local rules");
    Json result = Json::array();
    for (const auto& rule : rules) {
        const Json normalized{{"operation",localOperation(field(rule,"operation"))},
                              {"path",localPath(field(rule,"path")).string()}};
        if (std::ranges::find(result,normalized) == result.end()) result.push_back(normalized);
    }
    return result;
}

class File {
public:
    explicit File(int fd) : fd_(fd) {
        if (fd_ < 0) throw Conflict("Local path cannot be opened");
    }
    ~File() { if (fd_ >= 0) close(fd_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    void swap(File& other) { std::swap(fd_,other.fd_); }
    int fd() const { return fd_; }
    int release() { return std::exchange(fd_,-1); }
private:
    int fd_;
};

class HostLocalMachine final : public LocalMachine {
public:
    explicit HostLocalMachine(std::string owner) : owner_(std::move(owner)) {}
    std::string_view owner() const override { return owner_; }
    Json run(std::string_view operation, const std::filesystem::path& path) override {
        File file(open("/",O_RDONLY | O_DIRECTORY | O_CLOEXEC));
        const auto relative = path.relative_path();
        for (auto part = relative.begin(); part != relative.end(); ++part) {
            const bool directory = std::next(part) != relative.end() || operation == "list_directory";
            File next(openat(file.fd(),part->c_str(),O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC |
                             (directory ? O_DIRECTORY : 0)));
            file.swap(next);
        }
        if (operation == "read_file") {
            struct stat info{};
            if (fstat(file.fd(),&info) != 0 || !S_ISREG(info.st_mode)) throw Conflict("Expected a regular file");
            std::array<char,16001> bytes{};
            std::size_t size = 0;
            while (size < bytes.size()) {
                const auto n = read(file.fd(),bytes.data()+size,bytes.size()-size);
                if (n < 0) throw Conflict("Local file could not be read");
                if (n == 0) break;
                size += static_cast<std::size_t>(n);
            }
            auto content = Json(std::string(bytes.data(),std::min(size,std::size_t{16000})));
            return {{"text",Json::parse(content.dump(-1,' ',false,Json::error_handler_t::replace))},
                    {"truncated",size == bytes.size()}};
        }
        if (operation != "list_directory") throw Conflict("Unsupported local operation");
        const auto closeDirectory = [](DIR* descriptor) { closedir(descriptor); };
        auto dir = std::unique_ptr<DIR,decltype(closeDirectory)>(fdopendir(file.fd()),closeDirectory);
        if (!dir) throw Conflict("Local directory could not be opened");
        file.release();
        Json entries = Json::array();
        for (;;) {
            errno = 0;
            const auto* entry = readdir(dir.get());
            if (!entry) {
                if (errno) throw Conflict("Local directory could not be read");
                return {{"entries",entries},{"truncated",false}};
            }
            const std::string_view name(entry->d_name);
            if (name == "." || name == "..") continue;
            if (entries.size() == 100) return {{"entries",entries},{"truncated",true}};
            entries.push_back(Json::parse(Json(name).dump(-1,' ',false,Json::error_handler_t::replace)));
        }
    }
private:
    std::string owner_;
};
}

std::shared_ptr<LocalMachine> hostLocalMachine(std::string owner) {
    return std::make_shared<HostLocalMachine>(std::move(owner));
}

void Store::Impl::migrateLocalMachines() {
    exec(R"SQL(CREATE TABLE IF NOT EXISTS local_machine_connections(
      id INTEGER PRIMARY KEY, bot_id TEXT NOT NULL, task_id INTEGER NOT NULL REFERENCES tasks(id),
      environment_id TEXT NOT NULL, thread_id TEXT NOT NULL, requested_run_id TEXT NOT NULL,
      machine_id TEXT NOT NULL, machine_owner TEXT NOT NULL, rules TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN ('pending','approved','declined','disconnected')),
      decided_by TEXT, decided_at INTEGER))SQL");
    exec(R"SQL(CREATE UNIQUE INDEX IF NOT EXISTS local_machine_open_request
      ON local_machine_connections(bot_id,task_id,environment_id,thread_id,machine_id)
      WHERE status IN ('pending','approved'))SQL");
}

void Store::connectLocalMachine(std::string id, std::shared_ptr<LocalMachine> machine) {
    if (id.empty() || id.size() > 128 || !machine || machine->owner().empty()) throw Conflict("Invalid local machine");
    std::lock_guard lock(p_->mutex);
    p_->localMachines[std::move(id)] = std::move(machine);
}

void Store::setRootPeople(std::vector<std::string> ids) {
    std::lock_guard lock(p_->mutex);
    p_->rootPeople = std::move(ids);
}

void Store::setWebOrigin(std::string origin) {
    std::lock_guard lock(p_->mutex);
    p_->webOrigin = std::move(origin);
}

bool Store::Impl::permitsLocalMachineOwner(std::string_view owner, std::string_view bot) const {
    const Person person{std::string(owner),std::ranges::find(rootPeople,owner) != rootPeople.end()};
    return Access(const_cast<Impl&>(*this)).permitsPerson(person,bot,Role::decide);
}

Json Store::Impl::localMachineTool(std::string_view bot, std::string_view scope, const Decision& decision) {
    if (decision.tool != "request_local_machine_connection") return nullptr;
    const auto& args = decision.args;
    const auto thread = field(args,"threadId"), run = field(args,"runId"), machineId = field(args,"machineId");
    const auto machine = localMachines.find(machineId);
    if (machine == localMachines.end()) throw Conflict("Local machine is unavailable");
    const auto owner = machine->second->owner();
    if (!permitsLocalMachineOwner(owner,bot))
        throw ChannelAccessDenied("The machine owner cannot review requests for this Bot");
    auto session = query(R"SQL(
SELECT t.id,r.environment_id FROM runs r JOIN tasks t ON t.id=r.task_id
LEFT JOIN channel_tasks c ON c.task_id=t.id
WHERE t.bot_id=? AND r.thread_id=? AND r.run_id=?
AND (?='' OR c.scope_key=?)
AND t.state IN ('running','waiting')
AND json_extract(t.data,'$.activeRun.threadId')=r.thread_id
AND json_extract(t.data,'$.activeRun.runId')=r.run_id
AND json_extract(t.data,'$.environmentId')=r.environment_id LIMIT 2
)SQL",{bot,thread,run,scope,scope});
    if (!session.next()) throw Conflict("This Bot does not own that active Session");
    const auto task = session.number(0);
    const auto environment = session.string(1);
    if (session.next()) throw Conflict("The active Session is ambiguous");
    auto connection = query(R"SQL(
SELECT id,rules,status,machine_owner FROM local_machine_connections
WHERE bot_id=? AND task_id=? AND environment_id=? AND thread_id=? AND machine_id=? AND status IN ('pending','approved')
)SQL",{bot,task,environment,thread,machineId});
    const auto rules = localRules(args);
    redactor(bot).requirePublic({thread,run,machineId,owner,rules});
    Id id;
    std::string status = "pending";
    if (connection.next()) {
        if (connection.json(1) != rules || connection.string(3) != owner)
            throw Conflict("Resolve the existing request before changing its rules");
        id = connection.number(0);
        status = connection.string(2);
    } else {
        exec(R"SQL(
INSERT INTO local_machine_connections(bot_id,task_id,environment_id,thread_id,requested_run_id,machine_id,machine_owner,rules,status)
VALUES(?,?,?,?,?,?,?,?,'pending'))SQL",{bot,task,environment,thread,run,machineId,owner,rules});
        id = last();
    }
    Json result{{"ok",true},{"connectionId",id},{"status",status}};
    if (status == "pending") {
        if (!scope.empty() && webOrigin.empty())
            result["reviewInstructions"] = "On the Flock host, start Flock Web and open this Bot's Settings, then Access, then Local machine connections.";
        else
            result["reviewUrl"] = (scope.empty() ? std::string{} : webOrigin) + "/settings/" + std::string(bot) + "/access#local-connections";
    }
    return result;
}

Json Store::runLocalMachineOperation(std::string_view environment, const RunRef& authenticatedRun,
                                     std::string_view machineId, std::string_view operation,
                                     const std::filesystem::path& path) {
    const Json rule{{"operation",localOperation(operation)},{"path",localPath(path.native()).string()}};
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    const auto machine = p_->localMachines.find(machineId);
    if (machine == p_->localMachines.end()) throw Conflict("Local machine is unavailable");
    const auto owner = machine->second->owner();
    auto connection = p_->query(R"SQL(
SELECT c.bot_id,c.rules FROM runs r JOIN tasks t ON t.id=r.task_id
JOIN local_machine_connections c ON c.task_id=t.id AND c.bot_id=t.bot_id
AND c.environment_id=r.environment_id AND c.thread_id=r.thread_id
WHERE r.environment_id=? AND r.thread_id=? AND r.run_id=?
AND c.machine_id=? AND c.machine_owner=? AND c.status='approved'
AND t.state IN ('running','waiting')
AND json_extract(t.data,'$.activeRun.threadId')=r.thread_id
AND json_extract(t.data,'$.activeRun.runId')=r.run_id
AND json_extract(t.data,'$.environmentId')=r.environment_id
)SQL",{environment,authenticatedRun.threadId,authenticatedRun.runId,machineId,owner});
    if (!connection.next() || !p_->permitsLocalMachineOwner(owner,connection.string(0)))
        throw ChannelAccessDenied("The Session has no approved local connection");
    const auto bot = connection.string(0);
    const auto rules = connection.json(1);
    if (std::ranges::find(rules,rule) == rules.end()) throw ChannelAccessDenied("The operation is outside the approved local rules");
    auto output = p_->redactor(bot).payload(machine->second->run(operation,path));
    tx.commit();
    return output;
}

Json Store::localConnections(const Person& actor) const {
    std::lock_guard lock(p_->mutex);
    auto rows = p_->query(R"SQL(
SELECT id,bot_id,thread_id,requested_run_id,machine_id,machine_owner,rules,status
FROM local_machine_connections ORDER BY id DESC
)SQL");
    Json result = Json::array();
    while (rows.next()) {
        const auto bot = rows.string(1);
        if (rows.string(5) != actor.id || !Access(*p_).permitsPerson(actor,bot,Role::decide)) continue;
        result.push_back(p_->redactor(bot).record({{"id",rows.number(0)},{"botId",bot},{"threadId",rows.string(2)},
            {"runId",rows.string(3)},{"machineId",rows.string(4)},{"rules",rows.json(6)},{"status",rows.string(7)}}));
    }
    return result;
}

void Store::decideLocalConnection(const Person& actor, Id id, bool approve) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto row = p_->query(R"SQL(
SELECT c.bot_id,c.machine_id,c.machine_owner,c.status,t.state,t.data,c.thread_id,c.environment_id
FROM local_machine_connections c JOIN tasks t ON t.id=c.task_id WHERE c.id=?
)SQL",{id});
    if (!row.next() || row.string(2) != actor.id || !Access(*p_).permitsPerson(actor,row.string(0),Role::decide))
        throw ChannelAccessDenied("Only the machine owner can review this connection");
    if (row.string(3) != "pending") throw Conflict("Local connection is no longer pending");
    if (approve) {
        const auto machine = p_->localMachines.find(row.string(1));
        const auto task = row.json(5);
        const auto active = task.at("activeRun");
        if (machine == p_->localMachines.end() || machine->second->owner() != actor.id ||
            (row.string(4) != "running" && row.string(4) != "waiting") ||
            field(active,"threadId") != row.string(6) || field(active,"runId").empty() ||
            field(task,"environmentId") != row.string(7)) throw Conflict("Session or machine is no longer active");
    }
    p_->exec("UPDATE local_machine_connections SET status=?,decided_by=?,decided_at=? WHERE id=?",
             {approve ? "approved" : "declined",actor.id,unixTime(),id});
    tx.commit();
}

void Store::disconnectLocalConnection(const Person& actor, Id id) {
    std::lock_guard lock(p_->mutex);
    Transaction tx(p_->db);
    auto row = p_->query("SELECT bot_id,machine_owner,status FROM local_machine_connections WHERE id=?",{id});
    if (!row.next() || row.string(1) != actor.id || !Access(*p_).permitsPerson(actor,row.string(0),Role::decide))
        throw ChannelAccessDenied("Only the machine owner can disconnect this connection");
    if (row.string(2) != "approved") throw Conflict("Local connection is not approved");
    p_->exec("UPDATE local_machine_connections SET status='disconnected',decided_by=?,decided_at=? WHERE id=?",
             {actor.id,unixTime(),id});
    tx.commit();
}
}
