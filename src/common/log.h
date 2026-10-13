#pragma once
#include <windows.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace onyx {

inline std::mutex& LogMutex() { static std::mutex m; return m; }
inline FILE*& LogFile() { static FILE* f = nullptr; return f; }

// Creates the directory (and parents) and opens the log file for appending.
inline void LogInit(const std::wstring& dir, const std::wstring& fileName) {
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    _wfopen_s(&LogFile(), (dir + L"\\" + fileName).c_str(), L"ab");
}

inline void LogWrite(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::lock_guard<std::mutex> lk(LogMutex());
    for (FILE* out : {stderr, LogFile()}) {
        if (!out) continue;
        fprintf(out, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                st.wMinute, st.wSecond, st.wMilliseconds, msg);
        fflush(out);
    }
}

inline std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return (n > 0 && n < ARRAYSIZE(buf)) ? std::wstring(buf, n) : std::wstring();
}

inline std::wstring NarrowToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path, n);
    return p.substr(0, p.find_last_of(L'\\'));
}

}  // namespace onyx
