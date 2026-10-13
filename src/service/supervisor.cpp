#include "supervisor.h"

#include <sddl.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "../common/log.h"
#include "../common/protocol.h"

namespace onyx {
namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// Sessions that have a logged-on user. Active = connected, Disconnected = e.g. RDP closed.
// Whether a Disconnected session still renders and can be captured is exactly what the
// feasibility probe (tools/sessionprobe) answers; the supervisor does not decide that.
std::map<DWORD, std::wstring> EnumerateUserSessions() {
    std::map<DWORD, std::wstring> out;
    PWTS_SESSION_INFOW info = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count)) return out;
    for (DWORD i = 0; i < count; ++i) {
        const auto& s = info[i];
        if (s.SessionId == 0) continue;  // services session
        if (s.State != WTSActive && s.State != WTSDisconnected) continue;
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, s.SessionId, WTSUserName, &name, &bytes)) {
            if (name && *name) out[s.SessionId] = Lower(name);
            WTSFreeMemory(name);
        }
    }
    WTSFreeMemory(info);
    return out;
}

std::wstring TokenSidString(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) return L"";
    std::vector<BYTE> buf(size);
    if (!GetTokenInformation(token, TokenUser, buf.data(), size, &size)) return L"";
    LPWSTR str = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &str)) return L"";
    std::wstring r = str;
    LocalFree(str);
    return r;
}

SECURITY_ATTRIBUTES* PipeSecurity() {
    // SYSTEM and Administrators: full. Authenticated users: read/write (agents run as normal users).
    // The real access decision happens in HandleClient (pid + session must match a launched agent).
    static PSECURITY_DESCRIPTOR sd = [] {
        PSECURITY_DESCRIPTOR p = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
                                                             SDDL_REVISION_1, &p, nullptr);
        return p;
    }();
    static SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
    return &sa;
}

bool WriteMsg(HANDLE pipe, const char* msg) {
    DWORD written = 0;
    return WriteFile(pipe, msg, static_cast<DWORD>(strlen(msg)), &written, nullptr) != 0;
}

}  // namespace

Supervisor::Supervisor(std::wstring agentPath, std::set<std::wstring> allowedUsers)
    : agentPath_(std::move(agentPath)) {
    for (auto& u : allowedUsers) allowed_.insert(Lower(u));
}

Supervisor::~Supervisor() { Stop(); }

HANDLE Supervisor::CreatePipeInstance(bool first) {
    DWORD openMode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return CreateNamedPipeW(kPipeName, openMode,
                            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                            PIPE_UNLIMITED_INSTANCES, 512, 512, 0, PipeSecurity());
}

bool Supervisor::Start() {
    // FIRST_PIPE_INSTANCE fails if anyone else already owns the name (pipe squatting).
    HANDLE first = CreatePipeInstance(true);
    if (first == INVALID_HANDLE_VALUE) {
        LogWrite("FATAL: cannot create control pipe exclusively (error %lu)", GetLastError());
        return false;
    }
    if (allowed_.empty() && !warnedNoUsers_) {
        warnedNoUsers_ = true;
        LogWrite("WARN: allowed_users is empty in config.ini; no agents will be started");
    }
    running_ = true;
    pipeThread_ = std::thread([this, first] { PipeLoop(first); });
    worker_ = std::thread([this] { WorkerLoop(); });
    return true;
}

void Supervisor::Stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, a] : agents_) a.stopRequested = true;  // agents get "STOP" with their next heartbeat reply
    }
    for (int i = 0; i < 50; ++i) {  // up to ~5 s for graceful exit
        bool anyAlive = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& [id, a] : agents_)
                if (a.process && WaitForSingleObject(a.process, 0) == WAIT_TIMEOUT) anyAlive = true;
        }
        if (!anyAlive) break;
        Sleep(100);
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, a] : agents_) KillAgent(a);
    }
    // Unblock the blocking ConnectNamedPipe in PipeLoop.
    HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (pipeThread_.joinable()) pipeThread_.join();
    if (worker_.joinable()) worker_.join();
    for (int i = 0; i < 30 && activeClients_ > 0; ++i) Sleep(100);
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, a] : agents_) ReleaseHandles(a);
    agents_.clear();
}

void Supervisor::OnSessionChange(DWORD event, DWORD sessionId) {
    LogWrite("session event %lu for session %lu", event, sessionId);
    cv_.notify_all();
}

void Supervisor::WorkerLoop() {
    while (running_) {
        Tick();
        std::unique_lock<std::mutex> lk(cvMu_);
        cv_.wait_for(lk, std::chrono::seconds(1));
    }
}

void Supervisor::Tick() {
    auto live = EnumerateUserSessions();
    for (auto it = live.begin(); it != live.end();) {
        if (!allowed_.count(it->second)) it = live.erase(it);
        else ++it;
    }
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lk(mu_);

    for (auto it = agents_.begin(); it != agents_.end();) {
        auto l = live.find(it->first);
        if (it->second.dead || l == live.end() || l->second != it->second.user) {
            LogWrite("session %lu: dropping agent (session ended, user changed or identity mismatch)", it->first);
            KillAgent(it->second);
            ReleaseHandles(it->second);
            it = agents_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [sid, user] : live) {
        if (agents_.count(sid)) continue;
        Agent a;
        a.session = sid;
        a.user = user;
        agents_.emplace(sid, a);
        LogWrite("session %lu: user '%ls' is allowed, will start agent", sid, user.c_str());
    }
    for (auto& [sid, a] : agents_) Maintain(a, now);
}

void Supervisor::Maintain(Agent& a, ULONGLONG now) {
    if (a.process) {
        if (WaitForSingleObject(a.process, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(a.process, &code);
            LogWrite("session %lu: agent pid %lu exited with code %lu", a.session, a.pid, code);
            ReleaseHandles(a);
            a.connected = false;
            if (a.stopRequested) return;  // service is stopping, no restart
            unsigned backoff = std::min<unsigned>(30000u, 1000u << std::min(a.launchesInWindow, 5));
            a.nextStart = now + backoff;
            return;
        }
        if (a.connected && now - a.lastBeat > kHeartbeatTimeoutMs) {
            LogWrite("session %lu: agent pid %lu missed heartbeats, terminating", a.session, a.pid);
            TerminateProcess(a.process, 1);
        } else if (!a.connected && now - a.launchedAt > kHelloTimeoutMs) {
            LogWrite("session %lu: agent pid %lu never connected to the control pipe, terminating", a.session, a.pid);
            TerminateProcess(a.process, 1);
        }
        return;
    }
    if (a.stopRequested || now < a.nextStart) return;
    if (now - a.windowStart > 300000) {  // restart budget: 5 launches per 5 minutes
        a.windowStart = now;
        a.launchesInWindow = 0;
    }
    if (a.launchesInWindow >= 5) {
        a.nextStart = a.windowStart + 300000;
        LogWrite("session %lu: restart limit reached, pausing until the window ends", a.session);
        return;
    }
    ++a.launchesInWindow;
    if (!Launch(a)) a.nextStart = now + std::min<unsigned>(30000u, 2000u << std::min(a.launchesInWindow, 4));
}

bool Supervisor::Launch(Agent& a) {
    // WTSQueryUserToken needs SeTcbPrivilege, so this only works in the SYSTEM service.
    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(a.session, &userToken)) {
        LogWrite("session %lu: WTSQueryUserToken failed (%lu)", a.session, GetLastError());
        return false;
    }
    HANDLE primary = nullptr;
    BOOL ok = DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary);
    CloseHandle(userToken);
    if (!ok) {
        LogWrite("session %lu: DuplicateTokenEx failed (%lu)", a.session, GetLastError());
        return false;
    }

    std::wstring sid = TokenSidString(primary);
    if (sid.empty() || (!a.sid.empty() && a.sid != sid)) {
        LogWrite("session %lu: user SID changed or unreadable, refusing to launch", a.session);
        a.dead = true;
        CloseHandle(primary);
        return false;
    }
    a.sid = sid;

    LPVOID env = nullptr;
    if (!CreateEnvironmentBlock(&env, primary, FALSE)) {
        LogWrite("session %lu: CreateEnvironmentBlock failed (%lu)", a.session, GetLastError());
        CloseHandle(primary);
        return false;
    }

    std::wstring cmd = L"\"" + agentPath_ + L"\" --session " + std::to_wstring(a.session);
    std::wstring workDir = agentPath_.substr(0, agentPath_.find_last_of(L'\\'));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION pi{};
    // Suspended so the process joins its job object before it runs any code.
    ok = CreateProcessAsUserW(primary, agentPath_.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                              CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, env,
                              workDir.c_str(), &si, &pi);
    DWORD err = GetLastError();
    DestroyEnvironmentBlock(env);
    CloseHandle(primary);
    if (!ok) {
        LogWrite("session %lu: CreateProcessAsUser failed (%lu) for %ls", a.session, err, agentPath_.c_str());
        return false;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;  // agent dies if the service dies
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        if (!AssignProcessToJobObject(job, pi.hProcess)) LogWrite("session %lu: AssignProcessToJobObject failed (%lu)", a.session, GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    a.process = pi.hProcess;
    a.job = job;
    a.pid = pi.dwProcessId;
    a.launchedAt = GetTickCount64();
    a.connected = false;
    LogWrite("session %lu: started agent pid %lu for user '%ls'", a.session, a.pid, a.user.c_str());
    return true;
}

void Supervisor::KillAgent(Agent& a) {
    if (a.process && WaitForSingleObject(a.process, 0) == WAIT_TIMEOUT) TerminateProcess(a.process, 1);
}

void Supervisor::ReleaseHandles(Agent& a) {
    if (a.process) CloseHandle(a.process);
    if (a.job) CloseHandle(a.job);
    a.process = nullptr;
    a.job = nullptr;
    a.pid = 0;
}

void Supervisor::PipeLoop(HANDLE instance) {
    while (running_) {
        BOOL connected = ConnectNamedPipe(instance, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!running_ || !connected) {
            CloseHandle(instance);
            if (!running_) return;
        } else {
            ++activeClients_;
            std::thread([this, instance] {
                HandleClient(instance);
                --activeClients_;
            }).detach();
        }
        instance = CreatePipeInstance(false);
        while (instance == INVALID_HANDLE_VALUE && running_) {
            LogWrite("CreateNamedPipe failed (%lu), retrying", GetLastError());
            Sleep(1000);
            instance = CreatePipeInstance(false);
        }
        if (instance == INVALID_HANDLE_VALUE) return;
    }
    // Loop left via running_ == false before an instance was handed on: nothing to close here.
}

void Supervisor::HandleClient(HANDLE pipe) {
    ULONG clientPid = 0, clientSession = 0;
    GetNamedPipeClientProcessId(pipe, &clientPid);
    GetNamedPipeClientSessionId(pipe, &clientSession);

    auto reject = [&](const char* why) {
        LogWrite("rejected pipe client pid %lu session %lu: %s", clientPid, clientSession, why);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    };

    char buf[256];
    DWORD n = 0;
    if (!ReadFile(pipe, buf, sizeof buf - 1, &n, nullptr)) return reject("no hello");
    buf[n] = 0;
    if (strncmp(buf, "HELLO", 5) != 0) return reject("first message was not HELLO");

    {
        // Identity comes from the pipe, never from the message text.
        std::lock_guard<std::mutex> lk(mu_);
        auto it = agents_.find(clientSession);
        if (it == agents_.end() || !it->second.process || it->second.pid != clientPid || it->second.connected)
            return reject("not an agent launched by this service for that session");
        it->second.connected = true;
        it->second.lastBeat = GetTickCount64();
    }
    LogWrite("session %lu: agent pid %lu connected", clientSession, clientPid);
    if (!WriteMsg(pipe, "ACK")) goto done;

    for (;;) {
        if (!ReadFile(pipe, buf, sizeof buf - 1, &n, nullptr)) break;
        buf[n] = 0;
        if (strncmp(buf, "HB", 2) != 0) continue;
        bool stop = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = agents_.find(clientSession);
            if (it == agents_.end() || it->second.pid != clientPid) break;
            it->second.lastBeat = GetTickCount64();
            stop = it->second.stopRequested;
        }
        if (!WriteMsg(pipe, stop ? "STOP" : "OK")) break;
    }
done:
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = agents_.find(clientSession);
        if (it != agents_.end() && it->second.pid == clientPid) it->second.connected = false;
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}

}  // namespace onyx
