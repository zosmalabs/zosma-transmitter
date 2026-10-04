#include "tray_controller.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr wchar_t kMainWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT kTrayMessage = WM_APP + 20;
constexpr UINT kTrayIconId = 1;
constexpr int kIdStart = 1011;
constexpr int kIdTrayOpen = 1301;
constexpr int kIdTrayHide = 1302;
constexpr int kIdTrayStop = 1303;
constexpr int kIdTrayExit = 1304;

HWND gMain{};
HHOOK gHook{};
NOTIFYICONDATAW gTray{};
UINT gTaskbarCreated{};
std::atomic_bool gHidden{false};
std::mutex gFrameMutex;
std::vector<std::uint8_t> gHiddenFrame;
std::wstring gSourceName;

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring controlText(HWND hwnd) {
    if (!hwnd) return {};
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

bool transmissionRunning() {
    HWND start = gMain ? GetDlgItem(gMain, kIdStart) : nullptr;
    if (!start) return false;
    return controlText(start).find(L"Parar transmissão") != std::wstring::npos;
}

void addTrayIcon() {
    if (!gMain) return;
    gTray = {};
    gTray.cbSize = sizeof(gTray);
    gTray.hWnd = gMain;
    gTray.uID = kTrayIconId;
    gTray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    gTray.uCallbackMessage = kTrayMessage;
    gTray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (!gTray.hIcon) gTray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(gTray.szTip, L"Transmissor NDI Portátil");
    Shell_NotifyIconW(NIM_ADD, &gTray);
    gTray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &gTray);
}

void removeTrayIcon() {
    if (gTray.hWnd) Shell_NotifyIconW(NIM_DELETE, &gTray);
    gTray = {};
}

void showMainWindow() {
    if (!gMain) return;
    ShowWindow(gMain, SW_RESTORE);
    ShowWindow(gMain, SW_SHOW);
    SetForegroundWindow(gMain);
}

void showTrayMenu() {
    if (!gMain) return;
    const bool running = transmissionRunning();
    if (!running) gHidden = false;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kIdTrayOpen, L"Abrir");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (running ? MF_ENABLED : MF_GRAYED), kIdTrayHide,
                gHidden.load() ? L"Mostrar imagem" : L"Ocultar imagem");
    AppendMenuW(menu, MF_STRING | (running ? MF_ENABLED : MF_GRAYED), kIdTrayStop, L"Parar transmissão");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdTrayExit, L"Sair");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(gMain);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, gMain, nullptr);
    PostMessageW(gMain, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void buildHiddenFrameLocked() {
    constexpr int width = 1280;
    constexpr int height = 720;
    gHiddenFrame.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);

    HDC screen = GetDC(nullptr);
    HDC dc = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* dibPixels = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &dibPixels, nullptr, 0) : nullptr;
    if (!screen || !dc || !bitmap || !dibPixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT full{0, 0, width, height};
    HBRUSH background = CreateSolidBrush(RGB(15, 22, 33));
    FillRect(dc, &full, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfHeight = -38;
    lf.lfWeight = FW_SEMIBOLD;
    HFONT titleFont = CreateFontIndirectW(&lf);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, titleFont));
    SetTextColor(dc, RGB(245, 246, 248));
    RECT r{40, height / 2 - 100, width - 40, height / 2 - 20};
    DrawTextW(dc, L"Imagem ocultada", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    lf.lfHeight = -22;
    lf.lfWeight = FW_NORMAL;
    HFONT bodyFont = CreateFontIndirectW(&lf);
    SelectObject(dc, bodyFont);
    SetTextColor(dc, RGB(185, 196, 212));
    r = {40, height / 2 - 20, width - 40, height / 2 + 60};
    DrawTextW(dc, L"A imagem foi ocultada no computador transmissor.", -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (!gSourceName.empty()) {
        lf.lfHeight = -16;
        HFONT sourceFont = CreateFontIndirectW(&lf);
        SelectObject(dc, sourceFont);
        SetTextColor(dc, RGB(120, 136, 158));
        r = {40, height - 70, width - 40, height - 25};
        DrawTextW(dc, gSourceName.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DeleteObject(sourceFont);
    }

    std::memcpy(gHiddenFrame.data(), dibPixels, gHiddenFrame.size());
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    if (gTaskbarCreated && msg == gTaskbarCreated) {
        addTrayIcon();
        return 0;
    }

    if (msg == WM_SIZE && wp == SIZE_MINIMIZED) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }

    if (msg == kTrayMessage) {
        const UINT event = LOWORD(lp);
        if (event == WM_LBUTTONDBLCLK || event == WM_LBUTTONUP) showMainWindow();
        else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) showTrayMenu();
        return 0;
    }

    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == kIdTrayOpen) {
            showMainWindow();
            return 0;
        }
        if (id == kIdTrayHide) {
            if (transmissionRunning()) {
                gHidden = !gHidden.load();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        if (id == kIdTrayStop) {
            if (transmissionRunning()) {
                gHidden = false;
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(kIdStart, BN_CLICKED), 0);
            }
            return 0;
        }
        if (id == kIdTrayExit) {
            showMainWindow();
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (id == kIdStart) {
            // Toda nova transmissão começa mostrando a imagem; ao parar, a ocultação também é descartada.
            gHidden = false;
        }
    } else if (msg == WM_TIMER) {
        if (!transmissionRunning()) gHidden = false;
    } else if (msg == WM_NCDESTROY) {
        removeTrayIcon();
        gHidden = false;
        RemoveWindowSubclass(hwnd, subclassProc, 2);
        gMain = nullptr;
    }

    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installTray(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    SetWindowSubclass(hwnd, subclassProc, 2, 0);
    addTrayIcon();
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainWindowClass) == 0) installTray(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct TrayBootstrap {
    TrayBootstrap() {
        gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId());
    }
    ~TrayBootstrap() {
        if (gHook) UnhookWindowsHookEx(gHook);
    }
} gBootstrap;
}

bool trayImageHidden() {
    return gHidden.load();
}

void traySetImageHidden(bool hidden) {
    gHidden = hidden;
    if (gMain) InvalidateRect(gMain, nullptr, FALSE);
}

void traySetSourceName(const std::string& sourceNameUtf8) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    gSourceName = utf8ToWide(sourceNameUtf8);
    buildHiddenFrameLocked();
}

void trayResetTransmissionState() {
    gHidden = false;
}

bool trayHiddenFrame(const std::uint8_t*& data, int& width, int& height) {
    if (!gHidden.load()) return false;
    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (gHiddenFrame.empty()) buildHiddenFrameLocked();
    if (gHiddenFrame.empty()) return false;
    data = gHiddenFrame.data();
    width = 1280;
    height = 720;
    return true;
}
