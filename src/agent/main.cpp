// onyx_agent: runs inside one user session (started by onyx_service, never as SYSTEM).
// M1 scope: connect to the control pipe, prove its identity, send heartbeats, stop on request.
// Capture, encode, audio and input are added here in later milestones.
#include <windows.h>

#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <string>

#include "../common/log.h"
#include "../common/protocol.h"

using namespace onyx;

namespace {

// Refuse to talk to a pipe server that is not our own onyx_service.exe (pipe squatting defense).
bool ServerIsOurService(HANDLE pipe) {
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid)) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, serverPid);
    if (!proc) return false;  // fail closed
    wchar_t img[MAX_PATH * 2];
    DWORD len = ARRAYSIZE(img);
    bool ok = QueryFullProcessImageNameW(proc, 0, img, &len) != 0;
    CloseHandle(proc);
    if (!ok) return false;
    std::wstring expected = ExeDirectory() + L"\\" + kServiceExe;
    return _wcsicmp(img, expected.c_str()) == 0;
}

HANDLE ConnectToService() {
    for (int attempt = 0; attempt < 30; ++attempt) {
        // SECURITY_IDENTIFICATION: the server may learn who we are but must not impersonate us.
        HANDLE h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
            if (!ServerIsOurService(h)) {
                LogWrite("pipe server is not %ls, aborting", kServiceExe);
                CloseHandle(h);
                return INVALID_HANDLE_VALUE;
            }
            return h;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(kPipeName, 1000);
        else Sleep(1000);
    }
    return INVALID_HANDLE_VALUE;
}

bool Transact(HANDLE pipe, const char* msg, char* reply, DWORD replySize) {
    DWORD io = 0;
    if (!WriteFile(pipe, msg, static_cast<DWORD>(strlen(msg)), &io, nullptr)) return false;
    if (!ReadFile(pipe, reply, replySize - 1, &io, nullptr)) return false;
    reply[io] = 0;
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    DWORD actualSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &actualSession);
    DWORD requestedSession = actualSession;
    for (int i = 1; i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--session") == 0) requestedSession = static_cast<DWORD>(wcstoul(argv[i + 1], nullptr, 10));

    std::wstring local = EnvVar(L"LOCALAPPDATA");
    if (!local.empty()) LogInit(local + L"\\Onyx", L"agent-" + std::to_wstring(actualSession) + L".log");

    if (requestedSession != actualSession) {
        LogWrite("started with --session %lu but runs in session %lu, exiting", requestedSession, actualSession);
        return 2;
    }

    HANDLE pipe = ConnectToService();
    if (pipe == INVALID_HANDLE_VALUE) {
        LogWrite("could not connect to the service");
        return 3;
    }

    char reply[64];
    std::string hello = "HELLO " + std::to_string(actualSession) + " " + std::to_string(GetCurrentProcessId());
    if (!Transact(pipe, hello.c_str(), reply, sizeof reply) || strcmp(reply, "ACK") != 0) {
        LogWrite("service rejected the handshake");
        return 4;
    }
    LogWrite("agent running in session %lu", actualSession);

    for (;;) {
        if (!Transact(pipe, "HB", reply, sizeof reply)) {
            LogWrite("control pipe closed, exiting");
            return 5;
        }
        if (strcmp(reply, "STOP") == 0) {
            LogWrite("stop requested by service");
            return 0;
        }
        Sleep(kHeartbeatIntervalMs);  // later: capture/encode loop runs here
    }
}
