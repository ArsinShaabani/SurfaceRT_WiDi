// SurfaceRT-WiDi - the on-device receiver UI (Windows RT 8.1 / ARM, desktop app).
// Full screen, always-on-top-of-desktop wireless display surface.
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include <string>
#include <vector>

#include "image_decoder.h"
#include "presenter.h"
#include "widi/clock.h"
#include "widi/log.h"
#include "widi/sink_engine.h"
#include "widi/version.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")

namespace widi {
namespace app {

const wchar_t kWindowClass[] = L"SurfaceRTWiDiSinkWindow";
const wchar_t kMutexName[] = L"Global\\SurfaceRT-WiDi-Sink-Instance";
const wchar_t kAppTitle[] = L"SurfaceRT-WiDi";

struct Config {
    uint16_t tcpPort = kDefaultTcpPort;
    uint16_t discoveryPort = kDefaultDiscoveryPort;
    std::string name = kServiceName;
    bool forwardInput = true;
    bool beacons = true;
    bool windowed = false;
};

std::wstring install_dir() {
    wchar_t buffer[MAX_PATH] = {0};
    ::ExpandEnvironmentStringsW(L"%ProgramData%\\SurfaceRT-WiDi", buffer, MAX_PATH);
    return std::wstring(buffer);
}

Config load_config(const std::wstring& iniPath) {
    Config cfg;
    cfg.tcpPort = static_cast<uint16_t>(
        ::GetPrivateProfileIntW(L"sink", L"Port", kDefaultTcpPort, iniPath.c_str()));
    cfg.discoveryPort = static_cast<uint16_t>(
        ::GetPrivateProfileIntW(L"sink", L"DiscoveryPort", kDefaultDiscoveryPort, iniPath.c_str()));
    cfg.forwardInput =
        ::GetPrivateProfileIntW(L"sink", L"ForwardInput", 1, iniPath.c_str()) != 0;
    cfg.beacons = ::GetPrivateProfileIntW(L"sink", L"BroadcastBeacons", 1, iniPath.c_str()) != 0;
    cfg.windowed = ::GetPrivateProfileIntW(L"sink", L"Windowed", 0, iniPath.c_str()) != 0;

    wchar_t nameBuf[128] = {0};
    ::GetPrivateProfileStringW(L"sink", L"Name", L"Surface RT", nameBuf, 128, iniPath.c_str());
    char narrow[128] = {0};
    ::WideCharToMultiByte(CP_UTF8, 0, nameBuf, -1, narrow, sizeof(narrow), nullptr, nullptr);
    cfg.name = narrow[0] ? narrow : kServiceName;
    return cfg;
}

class SinkWindow : public ISinkHost, public IFrameConsumer {
public:
    SinkWindow() : engine_(*this) {}

    bool init(HINSTANCE instance, const Config& cfg, std::string* err);
    int run();
    void shutdown();

    // ISinkHost
    IFrameConsumer& frames() override { return *this; }
    void on_client_changed(bool connected, const std::string& who) override;

    // IFrameConsumer
    bool consume(const FrameInfo& info, const uint8_t* data, size_t size) override;
    void on_rate_changed(const RateSettings& settings) override;

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle_message(HWND, UINT, WPARAM, LPARAM);
    void update_status_text(bool force);
    void handle_touch(WPARAM, LPARAM);
    void handle_mouse(UINT msg, WPARAM, LPARAM);
    void send_pointer(uint16_t nx, uint16_t ny, uint8_t flags);
    void check_exit_hotspot(int x, int y);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    Config cfg_;
    SinkEngine engine_;
    ImageDecoder decoder_;
    Presenter presenter_;
    DecodedImage image_;
    DecodedImage lastImage_;
    std::wstring status_;
    uint32_t statusStamp_ = 0;
    uint32_t lastConnectChange_ = 0;
    int clientWidth_ = 0;
    int clientHeight_ = 0;
    int hotspotHits_ = 0;
    uint32_t hotspotStamp_ = 0;
    bool cursorHidden_ = false;
    bool keepAwake_ = false;
    uint32_t presented_ = 0;
    uint32_t dropped_ = 0;
};

}  // namespace app
}  // namespace widi

namespace widi {
namespace app {

bool SinkWindow::init(HINSTANCE instance, const Config& cfg, std::string* err) {
    instance_ = instance;
    cfg_ = cfg;
    if (!decoder_.init(err)) return false;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SinkWindow::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (err) *err = "RegisterClassEx failed";
        return false;
    }

    const int width = cfg_.windowed ? 1024 : ::GetSystemMetrics(SM_CXSCREEN);
    const int height = cfg_.windowed ? 640 : ::GetSystemMetrics(SM_CYSCREEN);

    hwnd_ = ::CreateWindowExW(cfg_.windowed ? WS_EX_APPWINDOW : WS_EX_TOOLWINDOW, kWindowClass,
                              kAppTitle, WS_POPUP | WS_VISIBLE, 0, 0, width, height, nullptr,
                              nullptr, instance, this);
    if (!hwnd_) {
        if (err) *err = "CreateWindowEx failed";
        return false;
    }

    // Touch must be handled explicitly, otherwise Windows synthesises mouse input
    // with a ~500 ms pen/tap delay.
    ::RegisterTouchWindow(hwnd_, 0);

    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    clientWidth_ = rc.right - rc.left;
    clientHeight_ = rc.bottom - rc.top;
    if (!presenter_.init(hwnd_, err)) return false;

    SinkConfig sinkCfg;
    sinkCfg.tcpPort = cfg_.tcpPort;
    sinkCfg.discoveryPort = cfg_.discoveryPort;
    sinkCfg.name = cfg_.name;
    sinkCfg.screenWidth = static_cast<uint16_t>(clientWidth_);
    sinkCfg.screenHeight = static_cast<uint16_t>(clientHeight_);
    sinkCfg.capabilities = static_cast<uint8_t>(kCapJpeg | kCapBgra8 | kCapInput | kCapTouch);
    sinkCfg.broadcastBeacons = cfg_.beacons;
    if (!engine_.start(sinkCfg, err)) return false;

    // A receiver that lets the screen dim is useless: hold the display awake.
    keepAwake_ = ::SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED |
                                          ES_SYSTEM_REQUIRED) != 0;
    update_status_text(true);
    WIDI_LOGI("sink window ready on %dx%d (%s backend)", clientWidth_, clientHeight_,
              presenter_.backend());
    return true;
}

int SinkWindow::run() {
    MSG msg;
    bool running = true;
    while (running) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (!running) break;
        engine_.pump(15);
        update_status_text(false);
    }
    return 0;
}

void SinkWindow::shutdown() {
    engine_.stop();
    presenter_.shutdown();
    decoder_.shutdown();
    if (cursorHidden_) {
        ::ShowCursor(TRUE);
        cursorHidden_ = false;
    }
    if (keepAwake_) {
        ::SetThreadExecutionState(ES_CONTINUOUS);
        keepAwake_ = false;
    }
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void SinkWindow::on_client_changed(bool connected, const std::string& who) {
    lastConnectChange_ = now_ms();
    if (connected) {
        presented_ = 0;
        dropped_ = 0;
        hotspotHits_ = 0;
        if (!cursorHidden_) {
            ::ShowCursor(FALSE);
            cursorHidden_ = true;
        }
        // Keep the surface above everything while a PC is projecting.
        ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        if (cursorHidden_) {
            ::ShowCursor(TRUE);
            cursorHidden_ = false;
        }
    }
    WIDI_LOGI("client %s (%s)", connected ? "attached" : "detached", who.c_str());
    update_status_text(true);
}

void SinkWindow::on_rate_changed(const RateSettings& settings) {
    (void)settings;
    update_status_text(false);
}

bool SinkWindow::consume(const FrameInfo& info, const uint8_t* data, size_t size) {
    std::string err;
    const bool ok = info.format == PixelFormat::Jpeg
                        ? decoder_.decode_jpeg(data, size, image_, &err)
                        : decoder_.copy_bgra(data, size, info.width, info.height, image_, &err);
    if (!ok) {
        ++dropped_;
        WIDI_LOGW("frame decode failed: %s", err.c_str());
        return false;
    }
    if (!presenter_.present(image_)) {
        ++dropped_;
        return false;
    }
    ++presented_;
    // Swap instead of copy: keeps WM_PAINT repaints cheap on Tegra 3.
    lastImage_.bgra.swap(image_.bgra);
    lastImage_.width = image_.width;
    lastImage_.height = image_.height;
    lastImage_.stride = image_.stride;
    return true;
}

}  // namespace app
}  // namespace widi

namespace widi {
namespace app {

LRESULT CALLBACK SinkWindow::wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SinkWindow* self =
        reinterpret_cast<SinkWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<SinkWindow*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
        return TRUE;
    }
    if (!self) return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->handle_message(hwnd, msg, wParam, lParam);
}

LRESULT SinkWindow::handle_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = ::BeginPaint(hwnd, &ps);
            if (!lastImage_.bgra.empty()) {
                presenter_.present(lastImage_);
            } else {
                RECT rc;
                ::GetClientRect(hwnd, &rc);
                ::FillRect(dc, &rc, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            clientWidth_ = LOWORD(lParam);
            clientHeight_ = HIWORD(lParam);
            presenter_.resize(static_cast<uint32_t>(clientWidth_),
                              static_cast<uint32_t>(clientHeight_));
            return 0;
        case WM_TOUCH:
            handle_touch(wParam, lParam);
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            handle_mouse(msg, wParam, lParam);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ::DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        case WM_DISPLAYCHANGE: {
            const int w = ::GetSystemMetrics(SM_CXSCREEN);
            const int h = ::GetSystemMetrics(SM_CYSCREEN);
            if (!cfg_.windowed) ::SetWindowPos(hwnd, HWND_TOP, 0, 0, w, h, SWP_SHOWWINDOW);
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void SinkWindow::send_pointer(uint16_t nx, uint16_t ny, uint8_t flags) {
    if (!cfg_.forwardInput) return;
    InputEvent ev;
    ev.kind = InputKind::PointerMove;
    ev.flags = flags;
    ev.x = nx;
    ev.y = ny;
    engine_.send_input(ev);
}

void SinkWindow::handle_touch(WPARAM wParam, LPARAM lParam) {
    const UINT reported = LOWORD(wParam);
    TOUCHINPUT inputs[10];
    const UINT count = reported > 10 ? 10 : reported;
    if (count == 0) return;
    if (!::GetTouchInputInfo(reinterpret_cast<HTOUCHINPUT>(lParam), count, inputs,
                             sizeof(TOUCHINPUT))) {
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        POINT pt;
        pt.x = TOUCH_COORD_TO_PIXEL(inputs[i].x);
        pt.y = TOUCH_COORD_TO_PIXEL(inputs[i].y);
        ::ScreenToClient(hwnd_, &pt);

        const bool down = (inputs[i].dwFlags & TOUCHEVENTF_DOWN) != 0;
        const bool up = (inputs[i].dwFlags & TOUCHEVENTF_UP) != 0;
        const uint16_t nx = clientWidth_ > 0
                                ? static_cast<uint16_t>(pt.x * 65535 / clientWidth_)
                                : 0;
        const uint16_t ny = clientHeight_ > 0
                                ? static_cast<uint16_t>(pt.y * 65535 / clientHeight_)
                                : 0;
        uint8_t flags = static_cast<uint8_t>(kInputTouch | kInputButtonLeft);
        if (!up) flags = static_cast<uint8_t>(flags | kInputDown);
        send_pointer(nx, ny, flags);
        if (down) check_exit_hotspot(pt.x, pt.y);
    }
    ::CloseTouchInputHandle(reinterpret_cast<HTOUCHINPUT>(lParam));
}

void SinkWindow::handle_mouse(UINT msg, WPARAM wParam, LPARAM lParam) {
    const int x = GET_X_LPARAM(lParam);
    const int y = GET_Y_LPARAM(lParam);
    const uint16_t nx = clientWidth_ > 0 ? static_cast<uint16_t>(x * 65535 / clientWidth_) : 0;
    const uint16_t ny = clientHeight_ > 0 ? static_cast<uint16_t>(y * 65535 / clientHeight_) : 0;

    if (msg == WM_MOUSEWHEEL) {
        if (!cfg_.forwardInput) return;
        // Wheel messages carry screen coordinates, not client coordinates.
        POINT wheel;
        wheel.x = GET_X_LPARAM(lParam);
        wheel.y = GET_Y_LPARAM(lParam);
        ::ScreenToClient(hwnd_, &wheel);
        InputEvent ev;
        ev.kind = InputKind::Wheel;
        ev.x = clientWidth_ > 0 ? static_cast<uint16_t>(wheel.x * 65535 / clientWidth_) : 0;
        ev.y = clientHeight_ > 0 ? static_cast<uint16_t>(wheel.y * 65535 / clientHeight_) : 0;
        ev.data = static_cast<uint32_t>(static_cast<int32_t>(GET_WHEEL_DELTA_WPARAM(wParam)));
        engine_.send_input(ev);
        return;
    }

    uint8_t flags = static_cast<uint8_t>(kInputButtonLeft);
    switch (msg) {
        case WM_LBUTTONDOWN:
            flags = static_cast<uint8_t>(kInputButtonLeft | kInputDown);
            check_exit_hotspot(x, y);
            break;
        case WM_LBUTTONUP:
            flags = static_cast<uint8_t>(kInputButtonLeft);
            break;
        case WM_RBUTTONDOWN:
            flags = static_cast<uint8_t>(kInputButtonRight | kInputDown);
            break;
        case WM_RBUTTONUP:
            flags = static_cast<uint8_t>(kInputButtonRight);
            break;
        default:
            // Plain moves are only useful while dragging; this keeps the
            // 2.4 GHz link free for video.
            if (!(wParam & MK_LBUTTON) && !(wParam & MK_RBUTTON)) return;
            flags = static_cast<uint8_t>(kInputButtonLeft);
            break;
    }
    send_pointer(nx, ny, flags);
}

}  // namespace app
}  // namespace widi

namespace widi {
namespace app {

void SinkWindow::check_exit_hotspot(int x, int y) {
    if (engine_.connected()) return;  // never quit while a PC is projecting
    const int size = 72;
    if (x < clientWidth_ - size || y < clientHeight_ - size) {
        hotspotHits_ = 0;
        return;
    }
    const uint32_t now = now_ms();
    if (hotspotStamp_ == 0 || now - hotspotStamp_ > 3000) hotspotHits_ = 0;
    hotspotStamp_ = now;
    if (++hotspotHits_ >= 3) {
        WIDI_LOGI("exit hotspot tapped three times; closing sink");
        ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

void SinkWindow::update_status_text(bool force) {
    const uint32_t now = now_ms();
    if (!force && now - statusStamp_ < 400) return;
    statusStamp_ = now;

    wchar_t buffer[320] = {0};
    const SinkStats& stats = engine_.stats();
    if (engine_.connected()) {
        swprintf_s(buffer, sizeof(buffer) / sizeof(buffer[0]),
                   L"%s   \x25CF  streaming  \x2014  %u fps  \x2014  %u ms  \x2014  q%u",
                   kAppTitle, static_cast<unsigned>(stats.presentedFps),
                   static_cast<unsigned>(stats.decodeMs),
                   static_cast<unsigned>(stats.rate.quality));
    } else {
        std::vector<std::string> ips;
        local_ipv4_addresses(ips);
        std::string ip = "?" ;
        for (size_t i = 0; i < ips.size(); ++i) {
            if (ips[i].compare(0, 4, "127.") != 0) {
                ip = ips[i];
                break;
            }
        }
        swprintf_s(buffer, sizeof(buffer) / sizeof(buffer[0]),
                   L"%s   \x25CB  waiting for PC  \x2014  %hs : %u", kAppTitle, ip.c_str(),
                   static_cast<unsigned>(engine_.config().tcpPort));
    }
    presenter_.set_status(std::wstring(buffer));
    if (engine_.connected() && !lastImage_.bgra.empty()) {
        // Refresh the HUD over the last picture without waiting for a new frame.
        presenter_.present(lastImage_);
    }
}

}  // namespace app
}  // namespace widi

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    using namespace widi;
    using namespace widi::app;

    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::wstring dir = install_dir();
    ::CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring logPath = dir + L"\\widi.log";
    char logNarrow[MAX_PATH * 2] = {0};
    ::WideCharToMultiByte(CP_UTF8, 0, logPath.c_str(), -1, logNarrow, sizeof(logNarrow), nullptr,
                          nullptr);
    set_log_level(LogLevel::Info);
    set_log_file(logNarrow);
    WIDI_LOGI("%s %s starting", WIDI_PRODUCT_NAME, WIDI_VERSION_STRING);

    // One receiver per machine.
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        WIDI_LOGW("another sink instance is already running; exiting");
        ::CloseHandle(mutex);
        return 0;
    }

    Config cfg = load_config(dir + L"\\widi.ini");
    for (int i = 1; i < __argc; ++i) {
        const std::wstring arg = __wargv[i];
        if (arg == L"--windowed") {
            cfg.windowed = true;
        } else if (arg == L"--no-beacon") {
            cfg.beacons = false;
        } else if (arg == L"--port" && i + 1 < __argc) {
            cfg.tcpPort = static_cast<uint16_t>(_wtoi(__wargv[++i]));
        }
    }

    SinkWindow window;
    std::string err;
    int rc = 0;
    if (!window.init(instance, cfg, &err)) {
        WIDI_LOGE("startup failed: %s", err.c_str());
        const std::wstring message = L"SurfaceRT-WiDi could not start:\n" +
                                     std::wstring(err.begin(), err.end()) +
                                     L"\n\nSee " + logPath;
        ::MessageBoxW(nullptr, message.c_str(), kAppTitle, MB_ICONERROR | MB_OK);
        rc = 1;
    } else {
        rc = window.run();
    }
    window.shutdown();
    if (mutex) ::CloseHandle(mutex);
    WIDI_LOGI("%s stopping", WIDI_PRODUCT_NAME);
    close_log_file();
    ::CoUninitialize();
    return rc;
}

#endif  // _WIN32
