// wireless-battery-tray: shows the battery level of wireless HID devices
// (mice, keyboards, headsets...) in the Windows notification area.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "HidDevice.h"
#include "TrayIcon.h"

namespace {

using BatteryList = std::vector<hid::BatteryInfo>;

constexpr UINT WM_BATTERY_RESULT = WM_APP + 2;
constexpr UINT kRefreshIntervalMs = 5 * 60 * 1000;  // battery levels change slowly

std::atomic<bool> g_refreshing{false};

// Runs on a thread-pool thread: HID queries can block on misbehaving
// devices, and the tray window must stay responsive.
DWORD WINAPI RefreshWorker(LPVOID context) {
    HWND hwnd = static_cast<HWND>(context);

    auto result = std::make_unique<BatteryList>();
    try {
        *result = hid::ReadAllBatteries();
    } catch (...) {
        result->clear();
    }

    if (PostMessageW(hwnd, WM_BATTERY_RESULT, 0, reinterpret_cast<LPARAM>(result.get()))) {
        result.release();  // ownership moves to the window message
    } else {
        g_refreshing = false;  // window is gone; nobody will receive the result
    }
    return 0;
}

void StartRefresh(HWND hwnd) {
    if (!hwnd || g_refreshing.exchange(true)) return;  // already running
    if (!QueueUserWorkItem(RefreshWorker, hwnd, WT_EXECUTELONGFUNCTION)) g_refreshing = false;
}

// szTip holds 127 characters, so keep whole lines and summarize the rest.
std::wstring BuildTooltip(const BatteryList& devices) {
    if (devices.empty()) return L"No wireless battery info found";

    constexpr std::size_t kMaxLength = 110;  // leaves room for "\n+NN more"
    std::wstring tip;
    std::size_t shown = 0;

    for (const hid::BatteryInfo& device : devices) {
        std::wstring name = device.name.size() > 28 ? device.name.substr(0, 27) + L"~" : device.name;
        const std::wstring line = name + L": " + std::to_wstring(device.percent) + L"%";

        const std::size_t separator = tip.empty() ? 0 : 1;
        if (tip.size() + separator + line.size() > kMaxLength) break;

        if (!tip.empty()) tip += L'\n';
        tip += line;
        ++shown;
    }

    if (shown < devices.size()) {
        tip += L"\n+" + std::to_wstring(devices.size() - shown) + L" more";
    }
    return tip;
}

void ApplyResult(TrayIcon& tray, BatteryList devices) {
    // Lowest battery first: it is what the icon shows and what matters most.
    std::sort(devices.begin(), devices.end(),
              [](const hid::BatteryInfo& a, const hid::BatteryInfo& b) {
                  return a.percent < b.percent;
              });

    const int lowest = devices.empty() ? -1 : devices.front().percent;
    tray.Update(lowest, BuildTooltip(devices));
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    // Allow only one copy per user session.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\wireless-battery-tray-single-instance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    int exitCode = 0;
    {
        TrayIcon tray(instance);

        TrayIcon::Callbacks callbacks;
        callbacks.onRefresh = [&tray] { StartRefresh(tray.Hwnd()); };
        callbacks.onMessage = [&tray](UINT msg, WPARAM, LPARAM lParam) -> bool {
            if (msg != WM_BATTERY_RESULT) return false;

            std::unique_ptr<BatteryList> result(reinterpret_cast<BatteryList*>(lParam));
            g_refreshing = false;
            if (result) ApplyResult(tray, std::move(*result));
            return true;
        };

        if (!tray.Create(std::move(callbacks), kRefreshIntervalMs)) {
            MessageBoxW(nullptr, L"Could not create the tray icon.", L"Wireless Battery Tray",
                        MB_OK | MB_ICONERROR);
            exitCode = 1;
        } else {
            StartRefresh(tray.Hwnd());

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            exitCode = static_cast<int>(msg.wParam);
        }
    }

    if (mutex) CloseHandle(mutex);
    return exitCode;
}
