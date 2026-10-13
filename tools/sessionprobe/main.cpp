// sessionprobe: M0a feasibility probe.
// Run once inside each Windows user session. Every second it writes one JSON line
// describing what the session can actually do: GPU rendering, desktop capture
// (DXGI Desktop Duplication), WASAPI loopback and session state.
// Usage: sessionprobe.exe [seconds]   (0 or omitted = run until the window is closed)
#define NOMINMAX
#include <windows.h>
#include <wtsapi32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowsapp.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::atomic<bool> g_running{true};
DWORD g_sessionId = 0;

// Shared counters, read once per second by the reporter.
std::atomic<long> g_renderFrames{0};
std::atomic<long> g_ddFrames{0}, g_ddTimeouts{0}, g_ddAccessLost{0}, g_ddRecreates{0};
std::atomic<long> g_ddCreateHr{1};   // 1 = not attempted yet
std::atomic<long> g_ddLastHr{0};
std::atomic<int> g_ddBrightness{-1}; // 0..255, -1 = no sample
std::atomic<long> g_audInitHr{1};
std::atomic<long> g_audPackets{0};
std::atomic<int> g_audPeakMilli{0};  // peak * 1000 since last report
std::atomic<long> g_beepErr{0};

std::string g_renderAdapter = "none";
std::atomic<bool> g_renderIsSoftware{false};
std::string g_audioDeviceId = "none";

std::string Narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string Json(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
    return o;
}

const char* WtsStateName(DWORD s) {
    switch (s) {
        case WTSActive: return "Active";
        case WTSConnected: return "Connected";
        case WTSConnectQuery: return "ConnectQuery";
        case WTSShadow: return "Shadow";
        case WTSDisconnected: return "Disconnected";
        case WTSIdle: return "Idle";
        case WTSListen: return "Listen";
        case WTSReset: return "Reset";
        case WTSDown: return "Down";
        case WTSInit: return "Init";
        default: return "Unknown";
    }
}

std::string QueryWtsState() {
    LPWSTR buf = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, g_sessionId, WTSConnectState, &buf, &bytes))
        return "QueryFailed:" + std::to_string(GetLastError());
    std::string r = WtsStateName(*reinterpret_cast<DWORD*>(buf));
    WTSFreeMemory(buf);
    return r;
}

// Name of the input desktop is not queryable from a disconnected/locked session without
// switching; we report only what we can ask without side effects.
std::string ListAdapters() {
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return "factory-failed";
    std::string out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        UINT outputs = 0;
        ComPtr<IDXGIOutput> o;
        while (a->EnumOutputs(outputs, &o) != DXGI_ERROR_NOT_FOUND) { ++outputs; o.Reset(); }
        if (!out.empty()) out += "; ";
        out += Narrow(d.Description) + " (vendor 0x" + [&] { char b[16]; snprintf(b, sizeof b, "%04X", d.VendorId); return std::string(b); }() +
               ", outputs " + std::to_string(outputs) + ")";
    }
    return out;
}

// ---------------------------------------------------------------- window + GPU render

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { g_running = false; PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

struct Renderer {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> sc;
    ComPtr<ID3D11RenderTargetView> rtv;

    bool Init(HWND hwnd) {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 640;
        sd.BufferDesc.Height = 360;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
        if (FAILED(hr)) {
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
            g_renderIsSoftware = true;
            if (FAILED(hr)) { g_renderAdapter = "device-creation-failed"; return false; }
        }
        ComPtr<IDXGIDevice> dxgiDev;
        ComPtr<IDXGIAdapter> ad;
        if (SUCCEEDED(dev.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
            DXGI_ADAPTER_DESC d{};
            ad->GetDesc(&d);
            g_renderAdapter = Narrow(d.Description);
            // 0x1414 = Microsoft (Basic Render Driver / WARP)
            if (d.VendorId == 0x1414) g_renderIsSoftware = true;
        }
        ComPtr<ID3D11Texture2D> bb;
        sc->GetBuffer(0, IID_PPV_ARGS(&bb));
        dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
        return true;
    }

    void Frame(long n) {
        if (!rtv) return;
        // Color depends on session id and frame counter so captured frames are attributable.
        float hue = static_cast<float>((g_sessionId * 67) % 360) / 360.0f;
        float pulse = 0.5f + 0.5f * static_cast<float>((n % 120) / 120.0);
        float c[4] = {hue * pulse, (1.0f - hue) * pulse, 0.3f + 0.7f * pulse, 1.0f};
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->ClearRenderTargetView(rtv.Get(), c);
        sc->Present(1, 0);
    }
};

// ---------------------------------------------------------------- desktop duplication

void DuplicationThread() {
    while (g_running) {
        ComPtr<IDXGIFactory1> f;
        ComPtr<IDXGIAdapter1> adapter;
        ComPtr<IDXGIOutput> output;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
            ComPtr<IDXGIAdapter1> a;
            for (UINT i = 0; !output && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
                ComPtr<IDXGIOutput> o;
                if (SUCCEEDED(a->EnumOutputs(0, &o))) { adapter = a; output = o; }
            }
        }
        if (!output) {
            g_ddCreateHr = static_cast<long>(DXGI_ERROR_NOT_FOUND); // no adapter has an output in this session
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        ComPtr<ID3D11Device> dev;
        ComPtr<ID3D11DeviceContext> ctx;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        ComPtr<IDXGIOutput1> out1;
        ComPtr<IDXGIOutputDuplication> dup;
        if (SUCCEEDED(hr)) hr = output.As(&out1);
        if (SUCCEEDED(hr)) hr = out1->DuplicateOutput(dev.Get(), &dup);
        g_ddCreateHr = static_cast<long>(hr);
        if (FAILED(hr)) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
        g_ddRecreates++;

        ComPtr<ID3D11Texture2D> staging;
        while (g_running) {
            DXGI_OUTDUPL_FRAME_INFO info{};
            ComPtr<IDXGIResource> res;
            hr = dup->AcquireNextFrame(100, &info, &res);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) { g_ddTimeouts++; continue; }
            if (FAILED(hr)) {
                g_ddLastHr = static_cast<long>(hr);
                if (hr == DXGI_ERROR_ACCESS_LOST) g_ddAccessLost++;
                break; // recreate the duplication
            }
            g_ddFrames++;
            if (info.LastPresentTime.QuadPart != 0) {
                ComPtr<ID3D11Texture2D> tex;
                if (SUCCEEDED(res.As(&tex))) {
                    D3D11_TEXTURE2D_DESC d{};
                    tex->GetDesc(&d);
                    if (!staging) {
                        D3D11_TEXTURE2D_DESC sd = d;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.BindFlags = 0;
                        sd.MiscFlags = 0;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        dev->CreateTexture2D(&sd, nullptr, &staging);
                    }
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (staging) {
                        ctx->CopyResource(staging.Get(), tex.Get());
                        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                            unsigned long long sum = 0, cnt = 0;
                            for (UINT y = 0; y < d.Height; y += 37) {
                                const unsigned char* row = static_cast<const unsigned char*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
                                for (UINT x = 0; x < d.Width; x += 37) {
                                    sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
                                    cnt += 3;
                                }
                            }
                            ctx->Unmap(staging.Get(), 0);
                            g_ddBrightness = cnt ? static_cast<int>(sum / cnt) : -1;
                        }
                    }
                }
            }
            dup->ReleaseFrame();
        }
    }
}

// ---------------------------------------------------------------- audio loopback + tone

void ToneThread() {
    DWORD freq = 440 + 110 * (g_sessionId % 6); // distinct per session
    while (g_running) {
        if (!Beep(freq, 400)) { g_beepErr = static_cast<long>(GetLastError()); std::this_thread::sleep_for(std::chrono::seconds(1)); }
    }
}

void AudioThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (g_running) {
        ComPtr<IMMDeviceEnumerator> en;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id))) { g_audioDeviceId = Narrow(id); CoTaskMemFree(id); }
        }
        ComPtr<IAudioClient> ac;
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac);
        WAVEFORMATEX* fmt = nullptr;
        if (SUCCEEDED(hr)) hr = ac->GetMixFormat(&fmt);
        if (SUCCEEDED(hr)) hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, fmt, nullptr);
        ComPtr<IAudioCaptureClient> cap;
        if (SUCCEEDED(hr)) hr = ac->GetService(IID_PPV_ARGS(&cap));
        if (SUCCEEDED(hr)) hr = ac->Start();
        g_audInitHr = static_cast<long>(hr);
        if (FAILED(hr)) {
            if (fmt) CoTaskMemFree(fmt);
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        const bool isFloat = fmt->wBitsPerSample == 32;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            UINT32 next = 0;
            hr = cap->GetNextPacketSize(&next);
            if (FAILED(hr)) break; // device lost, re-init
            while (next > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                g_audPackets++;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    float peak = 0;
                    size_t n = static_cast<size_t>(frames) * fmt->nChannels;
                    for (size_t i = 0; i < n; ++i) {
                        float v = isFloat ? reinterpret_cast<float*>(data)[i]
                                          : reinterpret_cast<short*>(data)[i] / 32768.0f;
                        if (v < 0) v = -v;
                        if (v > peak) peak = v;
                    }
                    int milli = static_cast<int>(peak * 1000);
                    int prev = g_audPeakMilli.load();
                    while (milli > prev && !g_audPeakMilli.compare_exchange_weak(prev, milli)) {}
                }
                cap->ReleaseBuffer(frames);
                if (FAILED(cap->GetNextPacketSize(&next))) break;
            }
        }
        ac->Stop();
        CoTaskMemFree(fmt);
    }
    CoUninitialize();
}

// ---------------------------------------------------------------- reporter

void Report(FILE* file, int sec) {
    char buf[1536];
    int n = snprintf(buf, sizeof buf,
        "{\"t\":%d,\"session\":%lu,\"wts\":\"%s\",\"remote\":%d,\"consoleSession\":%lu,"
        "\"render\":{\"adapter\":\"%s\",\"software\":%s,\"fps\":%ld},"
        "\"dd\":{\"createHr\":\"0x%08lX\",\"lastHr\":\"0x%08lX\",\"frames\":%ld,\"timeouts\":%ld,\"accessLost\":%ld,\"recreates\":%ld,\"brightness\":%d},"
        "\"audio\":{\"initHr\":\"0x%08lX\",\"device\":\"%s\",\"packets\":%ld,\"peak\":%.3f,\"beepErr\":%ld},"
        "\"wgcSupported\":%s}\n",
        sec, g_sessionId, QueryWtsState().c_str(), GetSystemMetrics(SM_REMOTESESSION) ? 1 : 0, WTSGetActiveConsoleSessionId(),
        Json(g_renderAdapter).c_str(), g_renderIsSoftware ? "true" : "false", g_renderFrames.exchange(0),
        static_cast<unsigned long>(g_ddCreateHr.load()), static_cast<unsigned long>(g_ddLastHr.load()),
        g_ddFrames.exchange(0), g_ddTimeouts.exchange(0), g_ddAccessLost.load(), g_ddRecreates.load(), g_ddBrightness.load(),
        static_cast<unsigned long>(g_audInitHr.load()), Json(g_audioDeviceId).c_str(), g_audPackets.exchange(0),
        g_audPeakMilli.exchange(0) / 1000.0, g_beepErr.load(),
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported() ? "true" : "false");
    if (n > 0) {
        fwrite(buf, 1, static_cast<size_t>(n), stdout);
        fflush(stdout);
        if (file) { fwrite(buf, 1, static_cast<size_t>(n), file); fflush(file); }
    }
}

} // namespace

int main(int argc, char** argv) {
    int duration = argc > 1 ? atoi(argv[1]) : 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &g_sessionId);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    char name[64];
    snprintf(name, sizeof name, "probe_session%lu.jsonl", g_sessionId);
    FILE* file = fopen(name, "ab");
    printf("{\"info\":\"adapters\",\"session\":%lu,\"list\":\"%s\"}\n", g_sessionId, Json(ListAdapters()).c_str());

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OnyxSessionProbe";
    RegisterClassW(&wc);
    std::wstring title = L"sessionprobe - session " + std::to_wstring(g_sessionId);
    HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 656, 399, nullptr, nullptr, wc.hInstance, nullptr);
    Renderer r;
    if (!hwnd || !r.Init(hwnd)) fprintf(stderr, "render init failed (see render.adapter in the log)\n");

    std::thread dd(DuplicationThread), audio(AudioThread), tone(ToneThread);

    auto start = std::chrono::steady_clock::now();
    auto lastReport = start;
    int sec = 0;
    MSG msg;
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        r.Frame(g_renderFrames.fetch_add(1));
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= std::chrono::seconds(1)) {
            lastReport += std::chrono::seconds(1);
            Report(file, ++sec);
        }
        if (duration > 0 && now - start >= std::chrono::seconds(duration)) g_running = false;
    }
    dd.join();
    audio.join();
    tone.join();
    if (file) fclose(file);
    return 0;
}
