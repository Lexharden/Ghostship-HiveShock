#include "HiveShock.h"

#include <libultraship.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "port/ShipInit.hpp"
#include "port/events/Events.h"
#include "port/ui/cvar_prefixes.h"
#include "sm64.h"
#include "game/level_update.h"

#if defined(__SWITCH__) || defined(__WIIU__)
#define HIVESHOCK_SOCKETS 0
#else
#define HIVESHOCK_SOCKETS 1
#endif

#if HIVESHOCK_SOCKETS
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static constexpr sock_t kBadSock = INVALID_SOCKET;
static void CloseSock(sock_t s) {
    closesocket(s);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using sock_t = int;
static constexpr sock_t kBadSock = -1;
static void CloseSock(sock_t s) {
    close(s);
}
#endif
#endif

extern "C" {
void hiveshock_effects_update(void);
int hiveshock_run_action(const char* action, double value, int hasValue, double duration);
int hiveshock_action_exists(const char* action);
int hiveshock_can_run(void);
const char* hiveshock_action_name(int index);
const char* hiveshock_action_category(int index);
int hiveshock_action_count(void);
}

namespace {

constexpr size_t kMaxQueued = 64;
constexpr size_t kMaxLine = 4096;
constexpr uint32_t kMaxClients = 8;

struct Command {
    std::string action;
    std::string user;
    double value = 0.0;
    bool hasValue = false;
    double duration = 0.0;
};

std::mutex sMutex; // guards the queue and the strings in sStatus
std::deque<Command> sQueue;
HiveShockStatus sStatus;
std::atomic<uint32_t> sClients{ 0 };
std::atomic<int> sDeaths{ 0 };
std::chrono::steady_clock::time_point sLastExec = std::chrono::steady_clock::now();

void Reject(const std::string& reason) {
    SPDLOG_WARN("HiveShock: rejected command ({})", reason);
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.rejected++;
    sStatus.lastRejected = reason;
}

// Parses one JSON line from the bridge and queues it. Runs on a network thread: no CVar access here
// except what the caller passes in.
void Enqueue(const std::string& line, const std::string& token) {
    auto json = nlohmann::json::parse(line, nullptr, false);
    if (!json.is_object() || !json.contains("action") || !json["action"].is_string()) {
        Reject("invalid JSON");
        return;
    }
    if (!token.empty() && !(json.contains("token") && json["token"].is_string() && json["token"] == token)) {
        Reject("bad token");
        return;
    }
    Command cmd;
    cmd.action = json["action"].get<std::string>();
    if (!hiveshock_action_exists(cmd.action.c_str())) {
        Reject("unknown action '" + cmd.action.substr(0, 40) + "'");
        return;
    }
    if (json.contains("user") && json["user"].is_string()) {
        cmd.user = json["user"].get<std::string>().substr(0, 64);
    }
    if (json.contains("value") && json["value"].is_number()) {
        cmd.value = json["value"].get<double>();
        cmd.hasValue = true;
    }
    if (json.contains("duration") && json["duration"].is_number()) {
        cmd.duration = json["duration"].get<double>();
    }
    std::lock_guard<std::mutex> lock(sMutex);
    if (sQueue.size() >= kMaxQueued) {
        sStatus.rejected++;
        sStatus.lastRejected = "queue full";
        return;
    }
    sQueue.push_back(std::move(cmd));
    sStatus.received++;
}

bool CategoryEnabled(const std::string& action) {
    for (int i = 0; i < hiveshock_action_count(); i++) {
        if (action == hiveshock_action_name(i)) {
            const char* category = hiveshock_action_category(i);
            if (category == nullptr) {
                // Alias: share the category of the first non-alias action with the same behavior is overkill;
                // aliases are always allowed when the master switch is on.
                return true;
            }
            const std::string cvar = std::string(CVAR_REMOTE("HiveShock.Cat.")) + category;
            return CVarGetInteger(cvar.c_str(), 1) != 0;
        }
    }
    return false;
}

// Runs on the game thread.
void Drain() {
    const int gapMs = CVarGetInteger(CVAR_REMOTE("HiveShock.MinGapMs"), 0);
    for (;;) {
        Command cmd;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sStatus.queued = (uint32_t) sQueue.size();
            if (sQueue.empty()) {
                return;
            }
            if (!hiveshock_can_run()) {
                return; // keep commands queued until Mario is playable (menus, warps, cutscenes)
            }
            const auto now = std::chrono::steady_clock::now();
            if (gapMs > 0 &&
                std::chrono::duration_cast<std::chrono::milliseconds>(now - sLastExec).count() < gapMs) {
                return;
            }
            cmd = std::move(sQueue.front());
            sQueue.pop_front();
            sStatus.queued = (uint32_t) sQueue.size();
        }

        if (!CategoryEnabled(cmd.action)) {
            Reject("category disabled for '" + cmd.action + "'");
            continue;
        }

        SPDLOG_INFO("HiveShock: {} <- {}", cmd.action, cmd.user.empty() ? "anon" : cmd.user);
        hiveshock_run_action(cmd.action.c_str(), cmd.value, cmd.hasValue ? 1 : 0, cmd.duration);
        sLastExec = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.executed++;
        sStatus.lastAction = cmd.action;
        sStatus.lastUser = cmd.user.empty() ? "anon" : cmd.user;
    }
}

#if HIVESHOCK_SOCKETS

class Server {
  public:
    ~Server() {
        Stop();
    }

    // Returns an empty string on success.
    std::string Start(int port, bool lan, std::string token) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        mToken = std::move(token);
        mFd = socket(AF_INET, SOCK_STREAM, 0);
        if (mFd == kBadSock) {
            return "socket() failed";
        }
        int yes = 1;
        setsockopt(mFd, SOL_SOCKET, SO_REUSEADDR, (const char*) &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t) port);
        addr.sin_addr.s_addr = htonl(lan ? INADDR_ANY : INADDR_LOOPBACK);
        if (bind(mFd, (sockaddr*) &addr, sizeof(addr)) != 0 || listen(mFd, 16) != 0) {
            CloseSock(mFd);
            mFd = kBadSock;
            return "cannot listen on port " + std::to_string(port) + " (in use?)";
        }
        mStop = false;
        mThread = std::thread([this]() { AcceptLoop(); });
        return "";
    }

    void Stop() {
        mStop = true;
        if (mThread.joinable()) {
            mThread.join();
        }
        if (mFd != kBadSock) {
            CloseSock(mFd);
            mFd = kBadSock;
        }
    }

  private:
    void AcceptLoop() {
        while (!mStop) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(mFd, &set);
            timeval tv{ 0, 200 * 1000 };
            if (select((int) mFd + 1, &set, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            sock_t client = accept(mFd, nullptr, nullptr);
            if (client == kBadSock) {
                continue;
            }
            if (sClients.load() >= kMaxClients) {
                CloseSock(client);
                continue;
            }
            sClients++;
            std::string token = mToken;
            std::thread([client, token]() {
                HandleClient(client, token);
                sClients--;
            }).detach();
        }
    }

    static void HandleClient(sock_t client, const std::string& token) {
#ifdef _WIN32
        DWORD timeoutMs = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeoutMs, sizeof(timeoutMs));
#else
        timeval tv{ 3, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*) &tv, sizeof(tv));
#endif
        std::string buffer;
        char chunk[512];
        int lines = 0;
        while (lines < 16) {
            const int n = recv(client, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                break;
            }
            buffer.append(chunk, (size_t) n);
            size_t nl;
            while ((nl = buffer.find('\n')) != std::string::npos && lines < 16) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty()) {
                    Enqueue(line, token);
                    lines++;
                }
            }
            if (buffer.size() > kMaxLine) {
                Reject("line too long");
                break;
            }
        }
        CloseSock(client);
    }

    sock_t mFd = kBadSock;
    std::atomic<bool> mStop{ false };
    std::thread mThread;
    std::string mToken;
};

std::unique_ptr<Server> sServer;

void SendEvent(const std::string& event, int deaths) {
    const int port = CVarGetInteger(CVAR_REMOTE("HiveShock.EventPort"), 43002);
    std::thread([event, deaths, port]() {
        sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd == kBadSock) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t) port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (sockaddr*) &addr, sizeof(addr)) == 0) { // bridge not running: best-effort
            const std::string payload = nlohmann::json({ { "event", event }, { "deaths", deaths } }).dump() + "\n";
            send(fd, payload.data(), (int) payload.size(), 0);
        }
        CloseSock(fd);
    }).detach();
}

#endif // HIVESHOCK_SOCKETS

} // namespace

bool HiveShock_IsSupported(void) {
    return HIVESHOCK_SOCKETS != 0;
}

void HiveShock_Apply(void) {
#if HIVESHOCK_SOCKETS
    sServer.reset(); // stops and joins

    const bool enabled = CVarGetInteger(CVAR_REMOTE("HiveShock.Enabled"), 0) != 0;
    const int port = std::clamp(CVarGetInteger(CVAR_REMOTE("HiveShock.Port"), 43000), 1024, 65535);
    const bool lan = CVarGetInteger(CVAR_REMOTE("HiveShock.AllowLAN"), 0) != 0;

    {
        std::lock_guard<std::mutex> lock(sMutex);
        sStatus.state = HiveShockState::Disabled;
        sStatus.port = port;
        sStatus.lan = lan;
        sStatus.lastError.clear();
    }
    if (!enabled) {
        return;
    }

    auto server = std::make_unique<Server>();
    const std::string err = server->Start(port, lan, CVarGetString(CVAR_REMOTE("HiveShock.Token"), ""));
    std::lock_guard<std::mutex> lock(sMutex);
    if (!err.empty()) {
        sStatus.state = HiveShockState::Error;
        sStatus.lastError = err;
        SPDLOG_ERROR("HiveShock: {}", err);
        return;
    }
    sServer = std::move(server);
    sStatus.state = HiveShockState::Listening;
    SPDLOG_INFO("HiveShock: listening on {}:{}", lan ? "0.0.0.0" : "127.0.0.1", port);
#else
    std::lock_guard<std::mutex> lock(sMutex);
    sStatus.state = HiveShockState::Unsupported;
#endif
}

HiveShockStatus HiveShock_GetStatus(void) {
    std::lock_guard<std::mutex> lock(sMutex);
    HiveShockStatus copy = sStatus;
    copy.queued = (uint32_t) sQueue.size();
    copy.clients = sClients.load();
    copy.deaths = sDeaths.load();
    return copy;
}

void HiveShock_QueueTest(const std::string& action) {
    std::lock_guard<std::mutex> lock(sMutex);
    if (sQueue.size() < kMaxQueued && hiveshock_action_exists(action.c_str())) {
        Command cmd;
        cmd.action = action;
        cmd.user = "Menu";
        sQueue.push_back(std::move(cmd));
        sStatus.received++;
    }
}

static void Init() {
    REGISTER_LISTENER(EngineReady, EVENT_PRIORITY_NORMAL, [](IEvent*) { HiveShock_Apply(); });
    REGISTER_LISTENER(GameFrameUpdate, EVENT_PRIORITY_NORMAL, [](IEvent*) {
        hiveshock_effects_update();
        Drain();
    });
    REGISTER_LISTENER(PlayerDeath, EVENT_PRIORITY_NORMAL, [](IEvent* event) {
        if (event->Cancelled) {
            return;
        }
        const int deaths = ++sDeaths;
#if HIVESHOCK_SOCKETS
        if (CVarGetInteger(CVAR_REMOTE("HiveShock.Enabled"), 0)) {
            SendEvent("player_death", deaths);
        }
#endif
    });
}

static RegisterShipInitFunc initFunc(Init);
