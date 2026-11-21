#pragma once

#include <windows.h>

#include <functional>
#include <string>

// Owns a hidden top-level window and one notification-area icon.
//
// The icon is drawn at runtime: a colored square with the battery percentage
// on it (green / orange / red, gray when unknown). The tooltip carries the
// full per-device list. Right click opens a menu (Refresh / Exit), left click
// triggers a refresh, and the icon is re-added automatically when Explorer
// restarts.
class TrayIcon {
public:
    struct Callbacks {
        // Called on timer ticks, on left click and on the "Refresh" menu item.
        std::function<void()> onRefresh;

        // Called for application messages (WM_APP .. 0xBFFF) that arrive at
        // the hidden window. Return true if the message was handled.
        std::function<bool(UINT, WPARAM, LPARAM)> onMessage;
    };

    explicit TrayIcon(HINSTANCE instance);
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // Creates the window and the icon and starts the refresh timer.
    bool Create(Callbacks callbacks, UINT refreshIntervalMs);

    // percent < 0 shows a gray "?". The tooltip is truncated to 127 chars.
    void Update(int percent, const std::wstring& tooltip);

    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool AddIcon();
    void RemoveIcon();
    void ShowContextMenu(int x, int y);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreatedMsg_ = 0;
    bool iconAdded_ = false;
    std::wstring tooltip_;
    Callbacks callbacks_;
};
