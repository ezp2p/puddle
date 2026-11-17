// platform/service.hpp for Windows: the service control manager, and processes without a console.

#include "platform/service.hpp"

#include <windows.h>

namespace puddle::platform {

namespace {
std::function<int()> g_body;
std::function<void()> g_stop;
SERVICE_STATUS_HANDLE g_status;

DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID)
{
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN)
        g_stop();
    return NO_ERROR;
}

void WINAPI serviceMain(DWORD, LPWSTR *)
{
    g_status = RegisterServiceCtrlHandlerExW(L"puddled", control, nullptr);
    // report SERVICE_RUNNING, run g_body(), report SERVICE_STOPPED with its result
}
} // namespace

int runService(const std::function<int()> &body, const std::function<void()> &stop)
{
    g_body = body;
    g_stop = stop;
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(L"puddled"), serviceMain }, { nullptr, nullptr } };
    if (StartServiceCtrlDispatcherW(table))
        return 0;
    // Not started by the service manager (the app or a terminal started it): run in place, stop on Ctrl-C.
    SetConsoleCtrlHandler([](DWORD) -> BOOL { g_stop(); return TRUE; }, TRUE);
    return body();
}

// installService: CreateServiceW; uninstallService: DeleteService
// runningPuddle: the lock file in the data directory, held with LockFileEx
// startDetached: CreateProcessW with DETACHED_PROCESS | CREATE_NO_WINDOW
// bundledPuddled: beside GetModuleFileNameW(nullptr)

} // namespace puddle::platform
