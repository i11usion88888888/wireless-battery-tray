// DIAGNOSTIC BUILD: logs to tray_log.txt next to the exe (falls back to %TEMP%
// if that folder is not writable) and, while kUseStockIconForDiagnostics is
// true, shows the stock application icon instead of the generated one.

#include "TrayIcon.h"

#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

// Set to false to go back to the generated percentage icon.
constexpr bool kUseStockIconForDiagnostics = false;

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kCmdRefresh = 1001;
constexpr UINT kCmdExit = 1002;
constexpr wchar_t kWindowClass[] = L"WirelessBatteryTrayWindow";

// ---------------------------------------------------------------- logging

std::wstring Pad(int value, int width) {
    std::wstring s = std::to_wstring(value);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), L'0');
    return s;
}

std::wstring LogPathBesideExe() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"tray_log.txt";  // current directory

    const std::wstring full(path, length);
    const std::size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring() : full.substr(0, slash + 1)) +
           L"tray_log.txt";
}

std::wstring LogPathInTemp() {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return L"tray_log.txt";
    return std::wstring(temp, length) + L"tray_log.txt";
}

bool AppendToFile(const std::wstring& path, const std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok =
        WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE;
}

void Log(const std::wstring& message) {
    static const std::wstring primary = LogPathBesideExe();

    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::wstring line = Pad(t.wYear, 4) + L"-" + Pad(t.wMonth, 2) + L"-" + Pad(t.wDay, 2) +
                              L" " + Pad(t.wHour, 2) + L":" + Pad(t.wMinute, 2) + L":" +
                              Pad(t.wSecond, 2) + L"." + Pad(t.wMilliseconds, 3) + L"  " +
                              message + L"\r\n";

    const int size = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return;
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), &utf8[0], size,
                        nullptr, nullptr);

    if (AppendToFile(primary, utf8)) return;
    static const std::wstring fallback = LogPathInTemp();
    AppendToFile(fallback, utf8);
}

// Call immediately after the failing API: it reads GetLastError() first.
void LogError(const wchar_t* what) {
    const DWORD error = GetLastError();
    Log(std::wstring(what) + L" FAILED, GetLastError=" + std::to_wstring(error));
}

std::wstring SingleLine(std::wstring text) {
    std::replace(text.begin(), text.end(), L'\n', L'|');
    return text;
}

// ------------------------------------------------------------------- icon

// LoadIconW(NULL, IDI_APPLICATION) returns a shared icon that must not be destroyed.
bool IsStockIcon(HICON icon) {
    return icon != nullptr && icon == LoadIconW(nullptr, IDI_APPLICATION);
}

COLORREF BackgroundFor(int percent) {
    if (percent < 0) return RGB(110, 110, 110);  // unknown
    if (percent < 20) return RGB(200, 40, 40);   // low
    if (percent < 40) return RGB(214, 120, 0);   // medium
    return RGB(0, 140, 70);                      // ok
}

// Draws the percentage onto a small square and turns it into an HICON.
// The caller owns the result and must DestroyIcon it. Returns nullptr on failure.
HICON CreatePercentIcon(int percent) {
    const int size = std::max(16, GetSystemMetrics(SM_CXSMICON));
    percent = std::min(percent, 100);

    HDC screen = GetDC(nullptr);
    if (!screen) {
        LogError(L"CreatePercentIcon: GetDC(NULL)");
        return nullptr;
    }

    HDC dc = CreateCompatibleDC(screen);
    if (!dc) {
        LogError(L"CreatePercentIcon: CreateCompatibleDC");
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) {
        LogError(L"CreatePercentIcon: CreateDIBSection");
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, color);

    RECT rect{0, 0, size, size};
    HBRUSH brush = CreateSolidBrush(BackgroundFor(percent));
    FillRect(dc, &rect, brush);
    DeleteObject(brush);

    const std::wstring text = percent < 0 ? L"?" : std::to_wstring(percent);
    const int fontHeight = text.size() >= 3 ? size * 11 / 20 : size * 3 / 4;
    HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!font) LogError(L"CreatePercentIcon: CreateFontW (non-fatal, no text)");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (font) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    GdiFlush();

    // GDI leaves the alpha channel at 0; make every pixel opaque.
    auto* pixels = static_cast<std::uint32_t*>(bits);
    for (int i = 0; i < size * size; ++i) pixels[i] |= 0xFF000000u;

    // All-zero AND mask: the color bitmap is shown as-is.
    const std::size_t maskBytes = static_cast<std::size_t>((size + 15) / 16) * 2 * size;
    std::vector<BYTE> maskBits(maskBytes, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    HICON icon = nullptr;
    if (!mask) {
        LogError(L"CreatePercentIcon: CreateBitmap (mask)");
    } else {
        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmMask = mask;
        info.hbmColor = color;
        icon = CreateIconIndirect(&info);
        if (!icon) LogError(L"CreatePercentIcon: CreateIconIndirect");
    }

    SelectObject(dc, oldBitmap);
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

void CopyTooltip(NOTIFYICONDATAW& nid, const std::wstring& text) {
    const std::size_t capacity = std::size(nid.szTip) - 1;
    const std::size_t length = std::min(text.size(), capacity);
    std::copy_n(text.begin(), length, nid.szTip);
    nid.szTip[length] = L'\0';
}

} // namespace

TrayIcon::TrayIcon(HINSTANCE instance) : instance_(instance) {}

TrayIcon::~TrayIcon() {
    if (hwnd_) DestroyWindow(hwnd_);
    if (icon_ && !IsStockIcon(icon_)) DestroyIcon(icon_);
}

bool TrayIcon::Create(Callbacks callbacks, UINT refreshIntervalMs) {
    callbacks_ = std::move(callbacks);

    Log(L"==== TrayIcon::Create (pid " + std::to_wstring(GetCurrentProcessId()) +
        L", stock icon mode: " + (kUseStockIconForDiagnostics ? L"ON" : L"OFF") + L")");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &TrayIcon::WndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogError(L"RegisterClassExW");
        return false;
    }

    // A normal (never shown) top-level window rather than a message-only
    // window: message-only windows do not receive the "TaskbarCreated"
    // broadcast, which we need to restore the icon after Explorer restarts.
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    if (taskbarCreatedMsg_ == 0) LogError(L"RegisterWindowMessageW(TaskbarCreated)");

    hwnd_ = CreateWindowExW(0, kWindowClass, L"Wireless Battery Tray", WS_OVERLAPPED, 0, 0, 0, 0,
                            nullptr, nullptr, instance_, this);
    if (!hwnd_) {
        LogError(L"CreateWindowExW");
        return false;
    }
    Log(L"Window created, hwnd=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd_)) +
        L", SM_CXSMICON=" + std::to_wstring(GetSystemMetrics(SM_CXSMICON)));

    // Always exercise the GDI icon code once, so the log shows whether it works,
    // even while the tray itself uses the stock icon.
    HICON generated = CreatePercentIcon(-1);
    Log(generated ? L"Test CreatePercentIcon(-1): OK" : L"Test CreatePercentIcon(-1): FAILED");

    if (kUseStockIconForDiagnostics) {
        if (generated) DestroyIcon(generated);
        icon_ = LoadIconW(nullptr, IDI_APPLICATION);
        if (!icon_) LogError(L"LoadIconW(IDI_APPLICATION)");
    } else {
        icon_ = generated;
    }
    if (!icon_) return false;

    tooltip_ = L"Wireless Battery Tray - scanning...";

    // If this fails, Explorer is probably not ready yet (e.g. right after
    // logon); the TaskbarCreated message will add the icon later.
    AddIcon();

    if (SetTimer(hwnd_, kTimerId, refreshIntervalMs, nullptr) == 0) {
        LogError(L"SetTimer");
    }
    return true;
}

void TrayIcon::Update(int percent, const std::wstring& tooltip) {
    if (!hwnd_) return;

    Log(L"Update: percent=" + std::to_wstring(percent) + L", tooltip=\"" + SingleLine(tooltip) +
        L"\", iconAdded=" + (iconAdded_ ? L"yes" : L"no"));

    // In diagnostic mode keep the stock icon so the test stays meaningful.
    HICON newIcon = kUseStockIconForDiagnostics ? icon_ : CreatePercentIcon(percent);
    if (!newIcon) {
        Log(L"Update: no new icon, keeping the previous one");
        return;  // keep the previous icon
    }

    HICON oldIcon = icon_;
    icon_ = newIcon;
    tooltip_ = tooltip;

    if (iconAdded_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_ICON | NIF_TIP;
        nid.hIcon = icon_;
        CopyTooltip(nid, tooltip_);
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) {
            LogError(L"Shell_NotifyIconW(NIM_MODIFY)");
            iconAdded_ = false;
            AddIcon();  // the icon may have vanished; try to add it again
        }
    } else {
        AddIcon();
    }

    if (oldIcon && oldIcon != icon_ && !IsStockIcon(oldIcon)) DestroyIcon(oldIcon);
}

bool TrayIcon::AddIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icon_;
    CopyTooltip(nid, tooltip_);

    Log(L"AddIcon: calling Shell_NotifyIconW(NIM_ADD), cbSize=" + std::to_wstring(nid.cbSize) +
        L", hwnd=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd_)) + L", IsWindow=" +
        (IsWindow(hwnd_) ? L"yes" : L"no") + L", hIcon=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(icon_)));

    SetLastError(0);
    iconAdded_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (!iconAdded_) {
        LogError(L"Shell_NotifyIconW(NIM_ADD)");
        return false;
    }
    Log(L"AddIcon: NIM_ADD returned TRUE");

    // Version 4 behavior: WM_CONTEXTMENU / NIN_SELECT notifications and
    // screen coordinates of the click in wParam.
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_SETVERSION, &nid)) {
        Log(L"AddIcon: NIM_SETVERSION(4) returned TRUE");
    } else {
        LogError(L"Shell_NotifyIconW(NIM_SETVERSION)");
    }
    return true;
}

void TrayIcon::RemoveIcon() {
    if (!iconAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayIconId;
    if (!Shell_NotifyIconW(NIM_DELETE, &nid)) LogError(L"Shell_NotifyIconW(NIM_DELETE)");
    iconAdded_ = false;
}

void TrayIcon::ShowContextMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        LogError(L"CreatePopupMenu");
        return;
    }
    AppendMenuW(menu, MF_STRING, kCmdRefresh, L"Refresh");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(hwnd_);
    const UINT command = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, hwnd_,
                       nullptr));
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    Log(L"Context menu closed, command=" + std::to_wstring(command));
    if (command == kCmdRefresh) {
        if (callbacks_.onRefresh) callbacks_.onRefresh();
    } else if (command == kCmdExit) {
        DestroyWindow(hwnd_);
    }
}

LRESULT CALLBACK TrayIcon::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    TrayIcon* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    return self->HandleMessage(hwnd, msg, wParam, lParam);
}

LRESULT TrayIcon::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMsg_ != 0 && msg == taskbarCreatedMsg_) {
        Log(L"TaskbarCreated received (Explorer started/restarted), re-adding icon");
        iconAdded_ = false;  // Explorer restarted; the old icon is gone
        AddIcon();
        return 0;
    }

    switch (msg) {
    case kTrayMessage:
        Log(L"Tray notification: event=" + std::to_wstring(LOWORD(lParam)) +
            L" (NIN_SELECT=1024, WM_CONTEXTMENU=123)");
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (callbacks_.onRefresh) callbacks_.onRefresh();
            break;
        case WM_CONTEXTMENU:
            ShowContextMenu(static_cast<int>(static_cast<short>(LOWORD(wParam))),
                            static_cast<int>(static_cast<short>(HIWORD(wParam))));
            break;
        default:
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == kTimerId && callbacks_.onRefresh) callbacks_.onRefresh();
        return 0;

    case WM_DESTROY:
        Log(L"WM_DESTROY: removing icon and quitting");
        KillTimer(hwnd_, kTimerId);
        RemoveIcon();
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;

    default:
        if (msg >= WM_APP && msg <= 0xBFFF && callbacks_.onMessage &&
            callbacks_.onMessage(msg, wParam, lParam)) {
            return 0;
        }
        break;
    }

    // Use the parameter, not hwnd_: hwnd_ is already null during WM_NCDESTROY.
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
