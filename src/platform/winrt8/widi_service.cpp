// SurfaceRT-WiDi - Windows service host (Windows RT 8.1 / ARM).
//
// A Windows service runs in session 0 and cannot show UI, so this host's only
// job is to keep widi_sink.exe alive inside the interactive console session:
// at boot, after every logon/unlock, and after any crash.
// SPDX-License-Identifier: MIT
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <stdio.h>
#include <wchar.h>

#include <string>
#include <vector>

#include "widi/log.h"
#include "widi/version.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")

namespace {

const wchar_t kServiceName[] = L"SurfaceRTWiDi";
const wchar_t kServiceDisplay[] = L"SurfaceRT-WiDi Wireless Display";
const wchar_t kSinkExe[] = L"widi_sink.exe";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {};
HANDLE g_stopEvent = nullptr;   // manual-reset: service stop / shutdown
HANDLE g_wakeEvent = nullptr;   // auto-reset: session change needs a re-check
HANDLE g_child = nullptr;
DWORD g_childPid = 0;
volatile LONG g_relaunchRequested = 0;
bool g_consoleMode = false;

std::wstring install_dir() {
    wchar_t buffer[MAX_PATH] = {0};
    ::ExpandEnvironmentStringsW(L"%ProgramData%\\SurfaceRT-WiDi", buffer, MAX_PATH);
    return std::wstring(buffer);
}

std::string to_utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr,
                                           nullptr);
    std::vector<char> buffer(static_cast<size_t>(need > 0 ? need : 1));
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, buffer.data(), need, nullptr, nullptr);
    return std::string(buffer.data());
}

void report_status(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        (state == SERVICE_RUNNING) ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                                      SERVICE_ACCEPT_SESSIONCHANGE)
                                   : 0;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        static DWORD checkPoint = 1;
        g_status.dwCheckPoint = checkPoint++;
    } else {
        g_status.dwCheckPoint = 0;
    }
    if (g_statusHandle) ::SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI control_handler(DWORD control, DWORD eventType, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report_status(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (g_stopEvent) ::SetEvent(g_stopEvent);
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            // Logon or unlock: the interactive session may have changed.
            if (eventType == WTS_SESSION_LOGON || eventType == WTS_SESSION_UNLOCK ||
                eventType == WTS_CONSOLE_CONNECT) {
                ::InterlockedExchange(&g_relaunchRequested, 1);
                if (g_wakeEvent) ::SetEvent(g_wakeEvent);
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            if (g_statusHandle) ::SetServiceStatus(g_statusHandle, &g_status);
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

struct FindWindowCtx {
    DWORD pid;
    HWND hwnd;
};

BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM param) {
    FindWindowCtx* ctx = reinterpret_cast<FindWindowCtx*>(param);
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid || !::IsWindowVisible(hwnd)) return TRUE;
    if (::GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    ctx->hwnd = hwnd;
    return FALSE;
}

}  // namespace

namespace {

bool launch_sink(std::wstring* error) {
    const DWORD session = ::WTSGetActiveConsoleSessionId();
    if (session == 0xFFFFFFFF) {
        if (error) *error = L"no active console session";
        return false;
    }

    HANDLE userToken = nullptr;
    if (!::WTSQueryUserToken(session, &userToken)) {
        if (error) *error = L"WTSQueryUserToken failed";
        return false;
    }

    HANDLE primaryToken = nullptr;
    const BOOL duplicated = ::DuplicateTokenEx(userToken, MAXIMUM_ALLOWED, nullptr,
                                              SecurityImpersonation, TokenPrimary,
                                              &primaryToken);
    ::CloseHandle(userToken);
    if (!duplicated) {
        if (error) *error = L"DuplicateTokenEx failed";
        return false;
    }

    LPVOID environment = nullptr;
    ::CreateEnvironmentBlock(&environment, primaryToken, FALSE);

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const std::wstring dir = install_dir();
    const std::wstring exe = dir + L"\\" + kSinkExe;
    std::wstring command = L"\"" + exe + L"\"";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);

    const BOOL created = ::CreateProcessAsUserW(
        primaryToken, exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_UNICODE_ENVIRONMENT, environment, dir.c_str(), &si, &pi);

    if (environment) ::DestroyEnvironmentBlock(environment);
    ::CloseHandle(primaryToken);

    if (!created) {
        if (error) {
            wchar_t buffer[256] = {0};
            swprintf_s(buffer, 256, L"CreateProcessAsUser failed (%lu)", ::GetLastError());
            *error = buffer;
        }
        return false;
    }

    ::CloseHandle(pi.hThread);
    g_child = pi.hProcess;
    g_childPid = pi.dwProcessId;
    WIDI_LOGI("sink launched in session %lu (pid %lu)", static_cast<unsigned long>(session),
              static_cast<unsigned long>(pi.dwProcessId));
    return true;
}

void stop_sink(bool graceful) {
    if (!g_child) return;
    if (graceful) {
        FindWindowCtx ctx = {g_childPid, nullptr};
        ::EnumWindows(&find_window_proc, reinterpret_cast<LPARAM>(&ctx));
        if (ctx.hwnd) {
            ::PostMessageW(ctx.hwnd, WM_CLOSE, 0, 0);
            if (::WaitForSingleObject(g_child, 4000) == WAIT_OBJECT_0) {
                ::CloseHandle(g_child);
                g_child = nullptr;
                return;
            }
        }
    }
    ::TerminateProcess(g_child, 0);
    ::WaitForSingleObject(g_child, 3000);
    ::CloseHandle(g_child);
    g_child = nullptr;
    WIDI_LOGW("sink process terminated");
}

}  // namespace

namespace {

void run_supervisor() {
    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    DWORD lastLaunchFailure = 0;

    for (;;) {
        if (::WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) break;
        const bool childAlive =
            g_child && ::WaitForSingleObject(g_child, 0) == WAIT_TIMEOUT;
        if (!childAlive) {
            if (g_child) {
                ::CloseHandle(g_child);
                g_child = nullptr;
                g_childPid = 0;
            }
            std::wstring error;
            if (!launch_sink(&error)) {
                const DWORD now = ::GetTickCount();
                if (lastLaunchFailure == 0 || now - lastLaunchFailure > 30000) {
                    WIDI_LOGW("cannot launch sink: %s", to_utf8(error).c_str());
                    lastLaunchFailure = now;
                }
            } else {
                lastLaunchFailure = 0;
            }
        }

        if (::InterlockedExchange(&g_relaunchRequested, 0) == 1) {
            // Session changed (logon/unlock): make sure the surface is on screen.
            if (g_child) {
                FindWindowCtx ctx = {g_childPid, nullptr};
                ::EnumWindows(&find_window_proc, reinterpret_cast<LPARAM>(&ctx));
                if (!ctx.hwnd) stop_sink(true);
            }
        }

        // 1 s poll: cheap, and keeps the service responsive to stop requests.
        // The wake event makes logon/unlock re-checks immediate.
        HANDLE waits[2] = {g_stopEvent, g_wakeEvent};
        if (::WaitForMultipleObjects(2, waits, FALSE, 1000) == WAIT_OBJECT_0) break;
    }

    stop_sink(true);
    if (g_wakeEvent) {
        ::CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
    if (g_stopEvent) {
        ::CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_statusHandle = ::RegisterServiceCtrlHandlerExW(kServiceName, control_handler, nullptr);
    if (!g_statusHandle) return;
    report_status(SERVICE_START_PENDING, NO_ERROR, 4000);

    const std::wstring logPath = install_dir() + L"\\widi-service.log";
    widi::set_log_level(widi::LogLevel::Info);
    widi::set_log_file(to_utf8(logPath).c_str());
    WIDI_LOGI("service starting (%s %s)", WIDI_PRODUCT_NAME, WIDI_VERSION_STRING);

    report_status(SERVICE_RUNNING);
    run_supervisor();

    WIDI_LOGI("service stopped");
    report_status(SERVICE_STOPPED);
    widi::close_log_file();
}

bool install_service(std::wstring* error) {
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        if (error) *error = L"OpenSCManager failed (run as administrator)";
        return false;
    }
    const std::wstring exe = install_dir() + L"\\widi_service.exe";
    SC_HANDLE service = ::CreateServiceW(
        scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, exe.c_str(), nullptr, nullptr, nullptr,
        L"LocalSystem", nullptr);
    if (!service) {
        const DWORD err = ::GetLastError();
        ::CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) {
            if (error) *error = L"service already installed";
        } else if (error) {
            wchar_t buffer[256] = {0};
            swprintf_s(buffer, 256, L"CreateService failed (%lu)", err);
            *error = buffer;
        }
        return false;
    }

    SERVICE_DESCRIPTIONW description;
    description.lpDescription =
        const_cast<LPWSTR>(L"Keeps the Surface RT wireless display receiver running in the "
                           L"interactive session so Windows 10/11 PCs can project to it.");
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    SC_ACTION actions[3];
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 5000;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 15000;
    actions[2].Type = SC_ACTION_NONE;
    actions[2].Delay = 0;
    SERVICE_FAILURE_ACTIONSW failure;
    ZeroMemory(&failure, sizeof(failure));
    failure.dwResetPeriod = 3600;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(scm);
    return true;
}

bool uninstall_service(std::wstring* error) {
    SC_HANDLE scm = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        if (error) *error = L"OpenSCManager failed";
        return false;
    }
    SC_HANDLE service = ::OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE);
    if (!service) {
        ::CloseServiceHandle(scm);
        if (error) *error = L"service is not installed";
        return false;
    }
    SERVICE_STATUS status;
    ::ControlService(service, SERVICE_CONTROL_STOP, &status);
    const BOOL deleted = ::DeleteService(service);
    ::CloseServiceHandle(service);
    ::CloseServiceHandle(scm);
    if (!deleted && error) *error = L"DeleteService failed";
    return deleted != FALSE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        std::wstring error;
        if (arg == L"--install") {
            if (install_service(&error)) {
                std::wprintf(L"installed. start with: sc start %ls\n", kServiceName);
                return 0;
            }
            std::wprintf(L"install failed: %ls\n", error.c_str());
            return 1;
        }
        if (arg == L"--uninstall") {
            if (uninstall_service(&error)) {
                std::wprintf(L"uninstalled\n");
                return 0;
            }
            std::wprintf(L"uninstall failed: %ls\n", error.c_str());
            return 1;
        }
        if (arg == L"--console") {
            // Debug mode: launch the sink and supervise it from a console.
            g_consoleMode = true;
            widi::set_log_level(widi::LogLevel::Debug);
            g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::wstring launchError;
            if (!launch_sink(&launchError)) {
                std::wprintf(L"launch failed: %ls\n", launchError.c_str());
                return 1;
            }
            while (g_child && ::WaitForSingleObject(g_child, 500) == WAIT_TIMEOUT) {
                if (::GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;
            }
            stop_sink(true);
            return 0;
        }
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), &service_main},
        {nullptr, nullptr},
    };
    if (!::StartServiceCtrlDispatcherW(table)) {
        const DWORD err = ::GetLastError();
        std::wprintf(L"StartServiceCtrlDispatcher failed (%lu).\n", err);
        std::wprintf(L"Run from an elevated prompt with --install, or use installer\\Install-WiDiService.ps1.\n");
        return 1;
    }
    return 0;
}

#endif  // _WIN32
