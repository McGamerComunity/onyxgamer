// onyx_service: manages one streaming agent per allowed Windows user session.
//   onyx_service.exe --install | --uninstall   register/remove the Windows service (admin)
//   onyx_service.exe --console                 run in the foreground (needs SYSTEM for WTSQueryUserToken,
//                                              e.g. started with PsExec -s)
//   onyx_service.exe --service                 used by the service control manager
#include <windows.h>
#include <wtsapi32.h>

#include <fstream>
#include <set>
#include <string>

#include "../common/log.h"
#include "../common/protocol.h"
#include "supervisor.h"

using namespace onyx;

namespace {

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status{};
HANDLE g_stopEvent = nullptr;
Supervisor* g_supervisor = nullptr;

std::wstring DataDir() {
    std::wstring pd = EnvVar(L"ProgramData");
    return (pd.empty() ? std::wstring(L"C:\\ProgramData") : pd) + L"\\Onyx";
}

// config.ini: one line  allowed_users=alice,bob   (account names without domain, case-insensitive)
std::set<std::wstring> LoadAllowedUsers() {
    std::set<std::wstring> users;
    std::ifstream in(DataDir() + L"\\config.ini");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("allowed_users=", 0) != 0) continue;
        std::string list = line.substr(14), cur;
        list += ',';
        for (char c : list) {
            if (c == ',') {
                while (!cur.empty() && (cur.back() == ' ' || cur.back() == '\r')) cur.pop_back();
                size_t start = cur.find_first_not_of(' ');
                if (start != std::string::npos) users.insert(NarrowToWide(cur.substr(start)));
                cur.clear();
            } else {
                cur += c;
            }
        }
    }
    return users;
}

void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE) : 0;
    g_status.dwWaitHint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 10000 : 0;
    if (g_statusHandle) SetServiceStatus(g_statusHandle, &g_status);
}

// Shared by service and console mode. Returns the process exit code.
int RunUntilStopped(bool asService) {
    LogInit(DataDir() + L"\\logs", L"service.log");
    Supervisor sup(ExeDirectory() + L"\\" + kAgentExe, LoadAllowedUsers());
    if (!sup.Start()) {
        if (asService) ReportStatus(SERVICE_STOPPED, ERROR_PIPE_BUSY);
        return 1;
    }
    g_supervisor = &sup;
    LogWrite("onyx_service started");
    if (asService) ReportStatus(SERVICE_RUNNING);
    WaitForSingleObject(g_stopEvent, INFINITE);
    LogWrite("onyx_service stopping");
    g_supervisor = nullptr;
    sup.Stop();
    return 0;
}

DWORD WINAPI ServiceCtrl(DWORD control, DWORD eventType, LPVOID eventData, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportStatus(SERVICE_STOP_PENDING);
            SetEvent(g_stopEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            if (g_supervisor && eventData)
                g_supervisor->OnSessionChange(eventType, static_cast<WTSSESSION_NOTIFICATION*>(eventData)->dwSessionId);
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrl, nullptr);
    if (!g_statusHandle) return;
    ReportStatus(SERVICE_START_PENDING);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    int rc = RunUntilStopped(true);
    ReportStatus(SERVICE_STOPPED, rc == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
}

BOOL WINAPI ConsoleCtrl(DWORD) {
    SetEvent(g_stopEvent);
    return TRUE;
}

int Install() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring bin = L"\"" + std::wstring(path) + L"\" --service";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { fprintf(stderr, "OpenSCManager failed (%lu). Run as administrator.\n", GetLastError()); return 1; }
    // LocalSystem on purpose: WTSQueryUserToken requires SeTcbPrivilege.
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"Onyx Streaming Service", SERVICE_ALL_ACCESS,
                                   SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                   bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc) { fprintf(stderr, "CreateService failed (%lu)\n", GetLastError()); CloseServiceHandle(scm); return 1; }
    printf("Service installed. Create %ls\\config.ini with  allowed_users=name1,name2  then: sc start %ls\n",
           DataDir().c_str(), kServiceName);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

int Uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return 1;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, DELETE | SERVICE_STOP);
    if (!svc) { fprintf(stderr, "OpenService failed (%lu)\n", GetLastError()); CloseServiceHandle(scm); return 1; }
    SERVICE_STATUS st{};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    int rc = DeleteService(svc) ? 0 : 1;
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return rc;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring arg = argc > 1 ? argv[1] : L"";
    if (arg == L"--install") return Install();
    if (arg == L"--uninstall") return Uninstall();
    if (arg == L"--console") {
        g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
        return RunUntilStopped(false);
    }
    if (arg == L"--service") {
        SERVICE_TABLE_ENTRYW table[] = {{const_cast<LPWSTR>(kServiceName), ServiceMain}, {nullptr, nullptr}};
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }
    fprintf(stderr, "usage: onyx_service --install | --uninstall | --console | --service\n");
    return 2;
}
