#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace onyx {

// Detects user sessions, runs one agent per allowed session in that user's context,
// restarts crashed agents with backoff, and accepts only agents it launched itself
// on the control pipe.
class Supervisor {
public:
    Supervisor(std::wstring agentPath, std::set<std::wstring> allowedUsers);
    ~Supervisor();

    bool Start();                      // false if the control pipe cannot be created exclusively
    void Stop();
    void OnSessionChange(DWORD event, DWORD sessionId);  // wakes the worker, no logic here

private:
    struct Agent {
        DWORD session = 0;
        std::wstring user;             // lowercase account name seen in the session
        std::wstring sid;              // user SID at first launch, checked on every relaunch
        HANDLE process = nullptr;
        HANDLE job = nullptr;
        DWORD pid = 0;
        ULONGLONG launchedAt = 0, lastBeat = 0, nextStart = 0, windowStart = 0;
        int launchesInWindow = 0;
        bool connected = false;
        bool stopRequested = false;
        bool dead = false;             // identity mismatch, drop entry
    };

    void WorkerLoop();
    void Tick();
    void Maintain(Agent& a, ULONGLONG now);
    bool Launch(Agent& a);
    void ReleaseHandles(Agent& a);
    void KillAgent(Agent& a);

    HANDLE CreatePipeInstance(bool first);
    void PipeLoop(HANDLE firstInstance);
    void HandleClient(HANDLE pipe);

    std::wstring agentPath_;
    std::set<std::wstring> allowed_;
    std::mutex mu_;
    std::map<DWORD, Agent> agents_;
    std::atomic<bool> running_{false};
    std::atomic<int> activeClients_{0};
    std::condition_variable cv_;
    std::mutex cvMu_;
    std::thread worker_, pipeThread_;
    bool warnedNoUsers_ = false;
};

}  // namespace onyx
