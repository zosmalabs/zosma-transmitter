#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Processing.NDI.Lib.h"

namespace {
using NdiLoadFunction = const NDIlib_v6* (*)();
using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"ZosmaReceiverWindowV1";
constexpr wchar_t kPreviewClass[] = L"ZosmaReceiverPreviewV1";
constexpr UINT kFrameReady = WM_APP + 1;
constexpr UINT kReceiverStopped = WM_APP + 2;
constexpr UINT_PTR kDiscoveryTimer = 100;

constexpr COLORREF kBackground = RGB(10, 15, 24);
constexpr COLORREF kPanel = RGB(19, 27, 39);
constexpr COLORREF kBorder = RGB(45, 58, 76);
constexpr COLORREF kText = RGB(241, 245, 249);
constexpr COLORREF kMuted = RGB(148, 163, 184);
constexpr COLORREF kAccent = RGB(38, 177, 120);

enum ControlId {
    IdSources = 1001,
    IdRefresh,
    IdConnect,
    IdFullscreen,
    IdPreview,
    IdStatus,
    IdDetails
};

struct SourceInfo {
    std::string name;
    std::string url;
};

struct State {
    HWND window{};
    HWND sources{};
    HWND refresh{};
    HWND connect{};
    HWND fullscreen{};
    HWND preview{};
    HWND status{};
    HWND details{};

    HMODULE ndiModule{};
    const NDIlib_v6* ndi{};
    NDIlib_find_instance_t finder{};
    NDIlib_recv_instance_t receiver{};

    std::vector<SourceInfo> sourceList;
    std::atomic_bool connected{false};
    std::atomic_bool stopRequested{false};
    std::thread worker;

    std::mutex frameMutex;
    std::vector<std::uint8_t> frame;
    int frameWidth{};
    int frameHeight{};
    int fps{};
    std::uint64_t framesReceived{};

    bool fullscreenMode{};
    WINDOWPLACEMENT previousPlacement{sizeof(WINDOWPLACEMENT)};
    DWORD previousStyle{};
};

State g;
HBRUSH gBackgroundBrush{};
HBRUSH gPanelBrush{};
HFONT gFont{};
HFONT gFontSmall{};
HFONT gFontBold{};
HFONT gFontTitle{};

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), length)).parent_path();
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

void setFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void setStatus(const std::wstring& status, const std::wstring& details = {}) {
    SetWindowTextW(g.status, status.c_str());
    SetWindowTextW(g.details, details.c_str());
}

bool loadNdi(std::wstring& error) {
    const auto runtime = executableDirectory() / L"Processing.NDI.Lib.x64.dll";
    g.ndiModule = LoadLibraryW(runtime.c_str());
    if (!g.ndiModule) {
        error = L"A DLL Processing.NDI.Lib.x64.dll não foi encontrada ao lado do programa.";
        return false;
    }

    auto load = reinterpret_cast<NdiLoadFunction>(GetProcAddress(g.ndiModule, "NDIlib_v6_load"));
    g.ndi = load ? load() : nullptr;
    if (!g.ndi || !g.ndi->initialize()) {
        error = L"Não foi possível inicializar o runtime NDI.";
        FreeLibrary(g.ndiModule);
        g.ndiModule = nullptr;
        g.ndi = nullptr;
        return false;
    }

    NDIlib_find_create_t settings{};
    settings.show_local_sources = true;
    g.finder = g.ndi->find_create_v2(&settings);
    if (!g.finder) {
        error = L"Não foi possível iniciar a descoberta de fontes NDI.";
        g.ndi->destroy();
        FreeLibrary(g.ndiModule);
        g.ndiModule = nullptr;
        g.ndi = nullptr;
        return false;
    }
    return true;
}

void refreshSources() {
    if (!g.finder || g.connected.load()) return;

    const int previous = static_cast<int>(SendMessageW(g.sources, CB_GETCURSEL, 0, 0));
    std::string previousName;
    if (previous >= 0 && previous < static_cast<int>(g.sourceList.size()))
        previousName = g.sourceList[previous].name;

    uint32_t count = 0;
    const NDIlib_source_t* found = g.ndi->find_get_current_sources(g.finder, &count);
    std::vector<SourceInfo> updated;
    updated.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        if (!found[index].p_ndi_name || !*found[index].p_ndi_name) continue;
        SourceInfo item;
        item.name = found[index].p_ndi_name;
        if (found[index].p_url_address) item.url = found[index].p_url_address;
        updated.push_back(std::move(item));
    }

    bool changed = updated.size() != g.sourceList.size();
    if (!changed) {
        for (size_t index = 0; index < updated.size(); ++index) {
            if (updated[index].name != g.sourceList[index].name || updated[index].url != g.sourceList[index].url) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    g.sourceList = std::move(updated);
    SendMessageW(g.sources, CB_RESETCONTENT, 0, 0);
    int selection = -1;
    for (size_t index = 0; index < g.sourceList.size(); ++index) {
        const std::wstring label = wide(g.sourceList[index].name);
        SendMessageW(g.sources, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (!previousName.empty() && previousName == g.sourceList[index].name)
            selection = static_cast<int>(index);
    }
    if (selection < 0 && !g.sourceList.empty()) selection = 0;
    if (selection >= 0) SendMessageW(g.sources, CB_SETCURSEL, selection, 0);

    if (g.sourceList.empty())
        setStatus(L"Procurando transmissores…", L"Os dois computadores precisam estar na mesma rede local.");
    else
        setStatus(L"Fonte encontrada", L"Selecione o transmissor e clique em Conectar.");
}

void receiveLoop() {
    auto started = std::chrono::steady_clock::now();
    int intervalFrames = 0;
    while (!g.stopRequested.load()) {
        NDIlib_video_frame_v2_t video{};
        const NDIlib_frame_type_e type = g.ndi->recv_capture_v3(g.receiver, &video, nullptr, nullptr, 100);
        if (type == NDIlib_frame_type_video) {
            if (video.p_data && video.xres > 0 && video.yres > 0) {
                const size_t rowBytes = static_cast<size_t>(video.xres) * 4u;
                std::vector<std::uint8_t> copy(rowBytes * static_cast<size_t>(video.yres));
                const auto* source = video.p_data;
                const int stride = video.line_stride_in_bytes != 0 ? video.line_stride_in_bytes : video.xres * 4;
                for (int row = 0; row < video.yres; ++row) {
                    const int sourceRow = stride >= 0 ? row : (video.yres - 1 - row);
                    std::copy_n(source + static_cast<std::ptrdiff_t>(sourceRow) * std::abs(stride), rowBytes,
                                copy.data() + static_cast<size_t>(row) * rowBytes);
                }
                {
                    std::lock_guard lock(g.frameMutex);
                    g.frame = std::move(copy);
                    g.frameWidth = video.xres;
                    g.frameHeight = video.yres;
                    ++g.framesReceived;
                }
                ++intervalFrames;
                const auto now = std::chrono::steady_clock::now();
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count();
                if (elapsed >= 1000) {
                    g.fps = static_cast<int>((intervalFrames * 1000LL) / std::max<std::int64_t>(1, elapsed));
                    intervalFrames = 0;
                    started = now;
                }
                PostMessageW(g.window, kFrameReady, 0, 0);
            }
            g.ndi->recv_free_video_v2(g.receiver, &video);
        } else if (type == NDIlib_frame_type_error) {
            break;
        }
    }
    PostMessageW(g.window, kReceiverStopped, 0, 0);
}

void disconnectReceiver(bool updateUi = true) {
    g.stopRequested = true;
    if (g.worker.joinable()) g.worker.join();
    if (g.ndi && g.receiver) g.ndi->recv_destroy(g.receiver);
    g.receiver = nullptr;
    g.connected = false;
    g.stopRequested = false;
    EnableWindow(g.sources, TRUE);
    EnableWindow(g.refresh, TRUE);
    SetWindowTextW(g.connect, L"Conectar");
    if (updateUi) setStatus(L"Desconectado", L"Escolha uma fonte para conectar novamente.");
}

void connectReceiver() {
    if (g.connected.load()) {
        disconnectReceiver();
        return;
    }

    const int selection = static_cast<int>(SendMessageW(g.sources, CB_GETCURSEL, 0, 0));
    if (selection < 0 || selection >= static_cast<int>(g.sourceList.size())) {
        setStatus(L"Nenhuma fonte selecionada", L"Clique em Atualizar e aguarde o transmissor aparecer.");
        return;
    }

    const SourceInfo& selected = g.sourceList[selection];
    NDIlib_recv_create_v3_t settings{};
    settings.source_to_connect_to.p_ndi_name = selected.name.c_str();
    settings.source_to_connect_to.p_url_address = selected.url.empty() ? nullptr : selected.url.c_str();
    settings.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    settings.bandwidth = NDIlib_recv_bandwidth_highest;
    settings.allow_video_fields = false;
    settings.p_ndi_recv_name = "Zosma Receiver";

    g.receiver = g.ndi->recv_create_v3(&settings);
    if (!g.receiver) {
        setStatus(L"Falha ao conectar", L"Não foi possível criar o receptor NDI.");
        return;
    }

    {
        std::lock_guard lock(g.frameMutex);
        g.frame.clear();
        g.frameWidth = 0;
        g.frameHeight = 0;
        g.framesReceived = 0;
        g.fps = 0;
    }
    g.connected = true;
    g.stopRequested = false;
    EnableWindow(g.sources, FALSE);
    EnableWindow(g.refresh, FALSE);
    SetWindowTextW(g.connect, L"Desconectar");
    setStatus(L"Conectando…", wide(selected.name));
    g.worker = std::thread(receiveLoop);
}

void toggleFullscreen() {
    if (!g.fullscreenMode) {
        g.previousStyle = static_cast<DWORD>(GetWindowLongPtrW(g.window, GWL_STYLE));
        GetWindowPlacement(g.window, &g.previousPlacement);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(g.window, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(g.window, GWL_STYLE, g.previousStyle & ~static_cast<DWORD>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(g.window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g.fullscreenMode = true;
    } else {
        SetWindowLongPtrW(g.window, GWL_STYLE, g.previousStyle);
        SetWindowPlacement(g.window, &g.previousPlacement);
        SetWindowPos(g.window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g.fullscreenMode = false;
    }
}

void layoutControls(int width, int height) {
    const int margin = 24;
    const int headerHeight = 76;
    const int controlsHeight = 76;
    const int statusHeight = 54;
    const int contentWidth = std::max(100, width - margin * 2);

    MoveWindow(g.sources, margin, headerHeight, std::max(180, contentWidth - 330), 38, TRUE);
    MoveWindow(g.refresh, width - margin - 318, headerHeight, 96, 38, TRUE);
    MoveWindow(g.connect, width - margin - 212, headerHeight, 116, 38, TRUE);
    MoveWindow(g.fullscreen, width - margin - 86, headerHeight, 86, 38, TRUE);

    const int previewTop = headerHeight + controlsHeight;
    const int previewHeight = std::max(120, height - previewTop - statusHeight - margin);
    MoveWindow(g.preview, margin, previewTop, contentWidth, previewHeight, TRUE);
    MoveWindow(g.status, margin, height - statusHeight, contentWidth / 2, 24, TRUE);
    MoveWindow(g.details, margin + contentWidth / 2, height - statusHeight, contentWidth / 2, 24, TRUE);
}

void paintPreview(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    FillRect(dc, &client, gPanelBrush);

    std::vector<std::uint8_t> frame;
    int sourceWidth = 0;
    int sourceHeight = 0;
    {
        std::lock_guard lock(g.frameMutex);
        frame = g.frame;
        sourceWidth = g.frameWidth;
        sourceHeight = g.frameHeight;
    }

    if (!frame.empty() && sourceWidth > 0 && sourceHeight > 0) {
        const int targetWidth = client.right - client.left;
        const int targetHeight = client.bottom - client.top;
        const double scale = std::min(static_cast<double>(targetWidth) / sourceWidth,
                                      static_cast<double>(targetHeight) / sourceHeight);
        const int drawWidth = std::max(1, static_cast<int>(sourceWidth * scale));
        const int drawHeight = std::max(1, static_cast<int>(sourceHeight * scale));
        const int x = (targetWidth - drawWidth) / 2;
        const int y = (targetHeight - drawHeight) / 2;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = sourceWidth;
        info.bmiHeader.biHeight = -sourceHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        StretchDIBits(dc, x, y, drawWidth, drawHeight, 0, 0, sourceWidth, sourceHeight,
                      frame.data(), &info, DIB_RGB_COLORS, SRCCOPY);
    } else {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, gMuted);
        SelectObject(dc, gFontBold);
        DrawTextW(dc, g.connected.load() ? L"Aguardando o primeiro quadro…" : L"Selecione uma fonte NDI para iniciar",
                  -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    FrameRect(dc, &client, GetSysColorBrush(COLOR_WINDOWFRAME));
    EndPaint(window, &paint);
}

LRESULT CALLBACK previewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        paintPreview(window);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g.window = window;
        g.sources = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdSources), nullptr, nullptr);
        g.refresh = CreateWindowExW(0, WC_BUTTONW, L"Atualizar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdRefresh), nullptr, nullptr);
        g.connect = CreateWindowExW(0, WC_BUTTONW, L"Conectar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdConnect), nullptr, nullptr);
        g.fullscreen = CreateWindowExW(0, WC_BUTTONW, L"Tela cheia", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdFullscreen), nullptr, nullptr);
        g.preview = CreateWindowExW(0, kPreviewClass, nullptr, WS_CHILD | WS_VISIBLE,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdPreview), nullptr, nullptr);
        g.status = CreateWindowExW(0, WC_STATICW, L"Inicializando…", WS_CHILD | WS_VISIBLE,
                                   0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdStatus), nullptr, nullptr);
        g.details = CreateWindowExW(0, WC_STATICW, L"Teste inicial de recepção de vídeo", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdDetails), nullptr, nullptr);

        for (HWND control : {g.sources, g.refresh, g.connect, g.fullscreen, g.status, g.details}) {
            setFont(control, gFont);
            SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
        }
        setFont(g.status, gFontBold);
        SetTimer(window, kDiscoveryTimer, 1000, nullptr);
        return 0;
    }
    case WM_SIZE:
        layoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdRefresh:
            refreshSources();
            return 0;
        case IdConnect:
            connectReceiver();
            return 0;
        case IdFullscreen:
            toggleFullscreen();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kDiscoveryTimer && !g.connected.load()) refreshSources();
        return 0;
    case kFrameReady: {
        InvalidateRect(g.preview, nullptr, FALSE);
        std::wstring details;
        {
            std::lock_guard lock(g.frameMutex);
            details = std::to_wstring(g.frameWidth) + L" × " + std::to_wstring(g.frameHeight) +
                      L"  •  " + std::to_wstring(g.fps) + L" FPS";
        }
        setStatus(L"Recebendo vídeo", details);
        return 0;
    }
    case kReceiverStopped:
        if (g.connected.load() && !g.stopRequested.load())
            setStatus(L"Sinal interrompido", L"O receptor tentará permanecer disponível; desconecte e conecte novamente.");
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && g.fullscreenMode) toggleFullscreen();
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kBackground);
        SetTextColor(dc, reinterpret_cast<HWND>(lParam) == g.status ? kText : kMuted);
        return reinterpret_cast<LRESULT>(gBackgroundBrush);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, gBackgroundBrush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        SelectObject(dc, gFontTitle);
        RECT title{24, 18, client.right - 24, 52};
        DrawTextW(dc, L"Zosma Receiver", -1, &title, DT_LEFT | DT_SINGLELINE);
        SetTextColor(dc, kAccent);
        SelectObject(dc, gFontSmall);
        RECT beta{24, 49, client.right - 24, 70};
        DrawTextW(dc, L"RECEPTOR NDI PORTÁTIL  •  PROTÓTIPO", -1, &beta, DT_LEFT | DT_SINGLELINE);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kDiscoveryTimer);
        disconnectReceiver(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    gBackgroundBrush = CreateSolidBrush(kBackground);
    gPanelBrush = CreateSolidBrush(kPanel);
    gFont = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    gFontSmall = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    gFontBold = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    gFontTitle = CreateFontW(-28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    WNDCLASSEXW previewClass{sizeof(WNDCLASSEXW)};
    previewClass.lpfnWndProc = previewProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.hbrBackground = gPanelBrush;
    previewClass.lpszClassName = kPreviewClass;
    RegisterClassExW(&previewClass);

    WNDCLASSEXW windowClass{sizeof(WNDCLASSEXW)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = gBackgroundBrush;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    std::wstring error;
    if (!loadNdi(error)) {
        MessageBoxW(nullptr, error.c_str(), L"Zosma Receiver", MB_OK | MB_ICONERROR);
        return 2;
    }

    HWND window = CreateWindowExW(0, kWindowClass, L"Zosma Receiver 0.1 Beta",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1040, 700,
                                  nullptr, nullptr, instance, nullptr);
    if (!window) return 3;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    refreshSources();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g.finder) g.ndi->find_destroy(g.finder);
    if (g.ndi) g.ndi->destroy();
    if (g.ndiModule) FreeLibrary(g.ndiModule);
    DeleteObject(gFontTitle);
    DeleteObject(gFontBold);
    DeleteObject(gFontSmall);
    DeleteObject(gFont);
    DeleteObject(gPanelBrush);
    DeleteObject(gBackgroundBrush);
    return static_cast<int>(message.wParam);
}
