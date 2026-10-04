#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_types.h"
#include "capture.h"
#include "ndi_sender.h"
#include "settings.h"
#include "tray_controller.h"

namespace {
using namespace std::chrono_literals;

constexpr wchar_t kWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr wchar_t kPreviewClass[] = L"TransmissorNDIPreviewV3";
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr UINT kWorkerEnded = WM_APP + 2;
constexpr UINT kReceiverMessage = WM_APP + 3;
constexpr UINT_PTR kPreviewTimer = 77;
constexpr auto kPrivacyScanInterval = 200ms;

constexpr COLORREF kBg = RGB(12, 18, 28);
constexpr COLORREF kPanel = RGB(24, 32, 45);
constexpr COLORREF kPanel2 = RGB(17, 24, 35);
constexpr COLORREF kBorder = RGB(46, 58, 76);
constexpr COLORREF kText = RGB(240, 244, 250);
constexpr COLORREF kMuted = RGB(158, 171, 191);

HBRUSH gBgBrush{};
HBRUSH gPanelBrush{};
HBRUSH gPanel2Brush{};
HBRUSH gEditBrush{};
HFONT gFont{};
HFONT gFontSmall{};
HFONT gFontBold{};
HFONT gFontTitle{};

enum ControlId {
    IdSourceName = 1001,
    IdCaptureKind,
    IdCaptureSource,
    IdRefresh,
    IdFps,
    IdCursor,
    IdProtected,
    IdQuick,
    IdAllowWhatsApp,
    IdAllowTelegram,
    IdStart,
    IdRelease,
    IdReceivers,
    IdStatus,
    IdHelp,
    IdPreview,
    IdManualPrivacy
};

enum class PrivateApp { None, WhatsApp, Telegram };

struct UiState {
    HWND window{};
    HWND sourceName{};
    HWND captureKind{};
    HWND captureSource{};
    HWND refresh{};
    HWND fps{};
    HWND cursor{};
    HWND protectedMode{};
    HWND quickMode{};
    HWND allowWhatsApp{};
    HWND allowTelegram{};
    HWND start{};
    HWND release{};
    HWND receivers{};
    HWND status{};
    HWND help{};
    HWND preview{};
    HWND manualPrivacy{};

    std::vector<MonitorSource> monitors;
    std::vector<WindowSource> windows;

    std::atomic_bool running{false};
    std::atomic_bool stopRequested{false};
    std::atomic_bool authorized{false};
    std::atomic_bool additionalConnectionLock{false};
    std::atomic_bool privacyBlocked{false};
    std::atomic_bool allowWa{false};
    std::atomic_bool allowTg{false};
    std::atomic_int rawConnections{0};
    std::atomic_int baselineConnections{0};
    bool activeProtectedMode{true};
    bool closeAfterWorker{false};

    std::thread worker;
    std::mutex workerMutex;
    AppSettings settings;

    std::vector<unsigned char> previewFrame;
    int previewWidth{0};
    int previewHeight{0};
    std::wstring previewLabel;
    bool previewProtected{false};
};

UiState g;

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path();
}

std::filesystem::path iniPath() { return executableDirectory() / L"transmissor-ndi.ini"; }

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring getText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    return text;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

bool contains(const std::wstring& text, const wchar_t* needle) {
    return lower(text).find(lower(std::wstring(needle))) != std::wstring::npos;
}

bool isChecked(HWND hwnd) {
    return SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void setChecked(HWND hwnd, bool checked) {
    SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

void postStatus(const std::wstring& text) {
    auto* copy = new std::wstring(text);
    if (!PostMessageW(g.window, kStatusMessage, 0, reinterpret_cast<LPARAM>(copy))) delete copy;
}

void postConnectionState(int rawCount) {
    PostMessageW(g.window, kReceiverMessage, static_cast<WPARAM>(rawCount), 0);
}

void applyDarkTheme(HWND hwnd) { SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr); }
void setControlFont(HWND hwnd, HFONT font) { SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); }

CaptureKind selectedKind() {
    return SendMessageW(g.captureKind, CB_GETCURSEL, 0, 0) == 1 ? CaptureKind::Window : CaptureKind::Monitor;
}

void refreshSources(int preferredIndex = -1) {
    SendMessageW(g.captureSource, CB_RESETCONTENT, 0, 0);
    const CaptureKind kind = selectedKind();
    if (kind == CaptureKind::Monitor) {
        g.monitors = enumerateMonitors();
        for (const auto& item : g.monitors)
            SendMessageW(g.captureSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    } else {
        g.windows = enumerateWindows(g.window);
        for (const auto& item : g.windows)
            SendMessageW(g.captureSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    }
    const LRESULT count = SendMessageW(g.captureSource, CB_GETCOUNT, 0, 0);
    if (count > 0) {
        const int index = preferredIndex >= 0 && preferredIndex < count ? preferredIndex : 0;
        SendMessageW(g.captureSource, CB_SETCURSEL, index, 0);
    }
    if (g.preview) InvalidateRect(g.preview, nullptr, TRUE);
}

CaptureSource currentSource() {
    CaptureSource source;
    source.kind = selectedKind();
    const int index = static_cast<int>(SendMessageW(g.captureSource, CB_GETCURSEL, 0, 0));
    if (source.kind == CaptureKind::Monitor && index >= 0 && index < static_cast<int>(g.monitors.size())) {
        const auto& item = g.monitors[index];
        source.monitor = item.handle;
        source.bounds = item.bounds;
        source.label = item.label;
    } else if (source.kind == CaptureKind::Window && index >= 0 && index < static_cast<int>(g.windows.size())) {
        const auto& item = g.windows[index];
        source.window = item.handle;
        source.bounds = item.bounds;
        source.label = item.label;
    }
    return source;
}

std::wstring processName(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD size = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result = std::filesystem::path(std::wstring(path, size)).filename().wstring();
    }
    CloseHandle(process);
    return lower(result);
}

PrivateApp classifyPrivateWindow(HWND hwnd) {
    if (!hwnd || hwnd == g.window) return PrivateApp::None;
    const std::wstring proc = processName(hwnd);
    const std::wstring title = lower(getText(hwnd));

    // Aplicativos empacotados e versões Beta/Business podem usar processos
    // hospedeiros (por exemplo, WebView2 ou ApplicationFrameHost). Por isso a
    // proteção não pode depender apenas do nome do executável: o título da
    // janela também é uma identidade válida para qualquer processo.
    if (proc.find(L"whatsapp") != std::wstring::npos ||
        title.find(L"whatsapp") != std::wstring::npos)
        return PrivateApp::WhatsApp;
    if (proc.find(L"telegram") != std::wstring::npos ||
        title.find(L"telegram") != std::wstring::npos)
        return PrivateApp::Telegram;

    // Alguns hosts exibem o título apenas na janela raiz ou proprietária.
    const HWND relatedWindows[] = {GetAncestor(hwnd, GA_ROOT), GetWindow(hwnd, GW_OWNER)};
    for (HWND candidate : relatedWindows) {
        if (!candidate || candidate == hwnd || candidate == g.window) continue;
        const std::wstring candidateProc = processName(candidate);
        const std::wstring candidateTitle = lower(getText(candidate));
        if (candidateProc.find(L"whatsapp") != std::wstring::npos ||
            candidateTitle.find(L"whatsapp") != std::wstring::npos)
            return PrivateApp::WhatsApp;
        if (candidateProc.find(L"telegram") != std::wstring::npos ||
            candidateTitle.find(L"telegram") != std::wstring::npos)
            return PrivateApp::Telegram;
    }
    return PrivateApp::None;
}

bool appAllowed(PrivateApp app) {
    if (app == PrivateApp::WhatsApp) return g.allowWa.load();
    if (app == PrivateApp::Telegram) return g.allowTg.load();
    return true;
}

bool windowActuallyVisibleInBounds(HWND hwnd, const RECT& captureBounds) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    RECT inter{};
    if (!IntersectRect(&inter, &wr, &captureBounds)) return false;

    const POINT points[] = {
        {(inter.left + inter.right) / 2, (inter.top + inter.bottom) / 2},
        {inter.left + 3, inter.top + 3},
        {inter.right - 3, inter.top + 3},
        {inter.left + 3, inter.bottom - 3},
        {inter.right - 3, inter.bottom - 3}
    };
    for (const POINT p : points) {
        HWND at = WindowFromPoint(p);
        HWND root = at ? GetAncestor(at, GA_ROOT) : nullptr;
        if (root == hwnd) return true;
    }
    return false;
}

struct PrivacyScanContext {
    RECT bounds{};
    bool blocked{false};
};

BOOL CALLBACK privacyEnumProc(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<PrivacyScanContext*>(lp);
    if (!ctx || ctx->blocked || hwnd == g.window) return TRUE;
    const PrivateApp app = classifyPrivateWindow(hwnd);
    if (app == PrivateApp::None || appAllowed(app)) return TRUE;
    if (windowActuallyVisibleInBounds(hwnd, ctx->bounds)) {
        ctx->blocked = true;
        return FALSE;
    }
    return TRUE;
}

bool protectedContentVisible(const CaptureSource& source) {
    if (source.kind == CaptureKind::Window) {
        const PrivateApp app = classifyPrivateWindow(source.window);
        return app != PrivateApp::None && !appAllowed(app);
    }
    PrivacyScanContext ctx{source.bounds, false};
    EnumWindows(privacyEnumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.blocked;
}

void collectSettings() {
    g.settings.sourceName = getText(g.sourceName);
    g.settings.captureKind = selectedKind();
    g.settings.sourceIndex = static_cast<int>(SendMessageW(g.captureSource, CB_GETCURSEL, 0, 0));
    g.settings.fps = SendMessageW(g.fps, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
    g.settings.showCursor = isChecked(g.cursor);
    g.settings.mode = isChecked(g.quickMode) ? TransmissionMode::Quick : TransmissionMode::Protected;
    saveSettings(iniPath(), g.settings);
}

void updateControls() {
    const bool running = g.running.load();
    const bool stopping = g.stopRequested.load();

    if (stopping) {
        EnableWindow(g.start, FALSE);
        SetWindowTextW(g.start, L"Parando...");
    } else {
        EnableWindow(g.start, TRUE);
        SetWindowTextW(g.start, running ? L"Parar transmissão" : L"Iniciar transmissão");
    }

    EnableWindow(g.sourceName, !running);
    EnableWindow(g.captureKind, !running);
    EnableWindow(g.captureSource, !running);
    EnableWindow(g.refresh, !running);
    EnableWindow(g.fps, !running);
    EnableWindow(g.cursor, !running);
    EnableWindow(g.protectedMode, !running);
    EnableWindow(g.quickMode, !running);
    EnableWindow(g.manualPrivacy, running && !stopping);
    if (g.manualPrivacy)
        SetWindowTextW(g.manualPrivacy, trayImageHidden() ? L"Mostrar imagem" : L"Ocultar imagem");

    const int raw = g.rawConnections.load();
    const int baseline = g.baselineConnections.load();
    const bool extraLock = g.additionalConnectionLock.load();
    const bool groupIsSafe = raw > 0 && (baseline == 0 || raw <= baseline);
    const bool canRelease = running && !stopping && g.activeProtectedMode && !g.authorized.load() && groupIsSafe;
    EnableWindow(g.release, canRelease);

    if (g.release) {
        if (g.authorized.load()) SetWindowTextW(g.release, L"Transmissão liberada ✓");
        else SetWindowTextW(g.release, extraLock ? L"Liberar novamente" : L"Liberar transmissão");
    }
}

std::vector<unsigned char> createMessageFrame(int width, int height,
                                               const std::wstring& title,
                                               const std::wstring& body,
                                               const std::wstring& sourceName) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* dibPixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &dibPixels, nullptr, 0);
    if (!bitmap || !dibPixels) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        return pixels;
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
    DrawTextW(dc, title.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    lf.lfHeight = -22;
    lf.lfWeight = FW_NORMAL;
    HFONT bodyFont = CreateFontIndirectW(&lf);
    SelectObject(dc, bodyFont);
    SetTextColor(dc, RGB(185, 196, 212));
    r = {40, height / 2 - 20, width - 40, height / 2 + 60};
    DrawTextW(dc, body.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    lf.lfHeight = -16;
    HFONT sourceFont = CreateFontIndirectW(&lf);
    SelectObject(dc, sourceFont);
    SetTextColor(dc, RGB(120, 136, 158));
    r = {40, height - 70, width - 40, height - 25};
    DrawTextW(dc, sourceName.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    std::memcpy(pixels.data(), dibPixels, pixels.size());
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(titleFont);
    DeleteObject(bodyFont);
    DeleteObject(sourceFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return pixels;
}

void updatePreview() {
    if (!g.preview || !IsWindowVisible(g.window)) return;
    CaptureSource source = currentSource();
    if (source.bounds.right <= source.bounds.left || source.bounds.bottom <= source.bounds.top) return;

    const bool blocked = trayImageHidden() || protectedContentVisible(source);
    g.previewProtected = blocked;
    if (blocked) {
        const bool manual = trayImageHidden();
        g.previewFrame = createMessageFrame(1280, 720,
            manual ? L"Imagem ocultada" : L"Conteúdo protegido",
            manual ? L"A privacidade manual está ativa." : L"Um aplicativo privado está aberto no monitor compartilhado.",
            source.label);
        g.previewWidth = 1280;
        g.previewHeight = 720;
        g.previewLabel = manual ? L"Privacidade manual" : L"Conteúdo protegido";
        InvalidateRect(g.preview, nullptr, FALSE);
        return;
    }

    std::vector<unsigned char> frame;
    int width = 0;
    int height = 0;
    if (captureToBuffer(source, isChecked(g.cursor), frame, width, height)) {
        g.previewFrame = std::move(frame);
        g.previewWidth = width;
        g.previewHeight = height;
        g.previewLabel = source.label;
        InvalidateRect(g.preview, nullptr, FALSE);
    }
}

void drawPreview(HDC dc, const RECT& client) {
    HBRUSH bg = CreateSolidBrush(RGB(8, 13, 21));
    FillRect(dc, &client, bg);
    DeleteObject(bg);
    if (g.previewFrame.empty() || g.previewWidth <= 0 || g.previewHeight <= 0) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kMuted);
        HFONT old = static_cast<HFONT>(SelectObject(dc, gFont));
        RECT text = client;
        DrawTextW(dc, L"Prévia indisponível", -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        return;
    }

    const int cw = client.right - client.left;
    const int ch = client.bottom - client.top;
    const double scale = std::min(static_cast<double>(cw) / g.previewWidth,
                                  static_cast<double>(ch) / g.previewHeight);
    const int dw = static_cast<int>(g.previewWidth * scale);
    const int dh = static_cast<int>(g.previewHeight * scale);
    const int dx = (cw - dw) / 2;
    const int dy = (ch - dh) / 2;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = g.previewWidth;
    info.bmiHeader.biHeight = -g.previewHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, g.previewWidth, g.previewHeight,
                  g.previewFrame.data(), &info, DIB_RGB_COLORS, SRCCOPY);

    RECT label{12, client.bottom - 38, client.right - 12, client.bottom - 10};
    HBRUSH overlay = CreateSolidBrush(RGB(16, 23, 34));
    FillRect(dc, &label, overlay);
    DeleteObject(overlay);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    HFONT old = static_cast<HFONT>(SelectObject(dc, gFontSmall));
    std::wstring text = g.previewLabel;
    if (!g.previewProtected && !text.empty())
        text += L"  ·  " + std::to_wstring(g.previewWidth) + L" × " + std::to_wstring(g.previewHeight);
    DrawTextW(dc, text.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, old);
}

void transmissionWorker(CaptureSource source, std::wstring sourceDisplayName,
                        std::string sourceName, int fps, bool showCursor,
                        TransmissionMode mode) {
    NdiSender sender;
    std::wstring error;
    if (!sender.open(executableDirectory() / L"Processing.NDI.Lib.x64.dll", sourceName, error)) {
        postStatus(L"Erro: " + error);
        g.running = false;
        PostMessageW(g.window, kWorkerEnded, 0, 0);
        return;
    }

    constexpr int placeholderWidth = 1280;
    constexpr int placeholderHeight = 720;
    const auto waitingFrame = createMessageFrame(placeholderWidth, placeholderHeight,
        L"Transmissão protegida", L"Aguardando autorização no computador transmissor.", sourceDisplayName);
    const auto extraConnectionFrame = createMessageFrame(placeholderWidth, placeholderHeight,
        L"Transmissão temporariamente bloqueada", L"Foi detectada uma conexão adicional.", sourceDisplayName);
    const auto privacyFrame = createMessageFrame(placeholderWidth, placeholderHeight,
        L"Conteúdo protegido", L"Um aplicativo privado está aberto no monitor compartilhado.", sourceDisplayName);

    g.authorized = mode == TransmissionMode::Quick;
    g.additionalConnectionLock = false;
    g.privacyBlocked = false;
    g.rawConnections = 0;
    g.baselineConnections = 0;
    postConnectionState(0);
    postStatus(mode == TransmissionMode::Protected ? L"Aguardando receptor" : L"Transmitindo");

    std::vector<unsigned char> frame;
    int width = 0;
    int height = 0;
    int previousConnections = -1;
    bool privacy = trayImageHidden() || protectedContentVisible(source);
    bool previousPrivacy = privacy;
    g.privacyBlocked = privacy;
    auto nextPrivacyScan = std::chrono::steady_clock::now() + kPrivacyScanInterval;
    auto nextFrame = std::chrono::steady_clock::now();

    while (!g.stopRequested.load()) {
        const int connections = std::max(0, sender.connections());
        if (connections != previousConnections) {
            previousConnections = connections;
            g.rawConnections = connections;
            postConnectionState(connections);
            if (mode == TransmissionMode::Protected) {
                const int baseline = g.baselineConnections.load();
                if (connections == 0) {
                    g.authorized = false;
                    g.additionalConnectionLock = false;
                    g.baselineConnections = 0;
                    postStatus(L"Aguardando receptor");
                } else if (baseline > 0 && connections > baseline) {
                    g.authorized = false;
                    g.additionalConnectionLock = true;
                    postStatus(L"Bloqueada · conexão adicional");
                } else if (!g.authorized.load()) {
                    postStatus(g.additionalConnectionLock.load() ? L"Aguardando nova liberação" : L"Receptor aguardando liberação");
                }
            } else {
                postStatus(connections > 0 ? L"Transmitindo" : L"Ativa · sem receptor");
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPrivacyScan) {
            privacy = trayImageHidden() || protectedContentVisible(source);
            nextPrivacyScan = now + kPrivacyScanInterval;
        }
        g.privacyBlocked = privacy;
        if (privacy != previousPrivacy) {
            previousPrivacy = privacy;
            if (privacy) postStatus(trayImageHidden() ? L"Privacidade manual · imagem e áudio ocultos" : L"Privacidade · conteúdo protegido");
            else if (mode == TransmissionMode::Protected && !g.authorized.load()) postStatus(L"Receptor aguardando liberação");
            else postStatus(L"Transmitindo");
            InvalidateRect(g.window, nullptr, FALSE);
        }

        if (privacy) {
            if (!sender.sendFrame(privacyFrame.data(), placeholderWidth, placeholderHeight, fps)) break;
        } else if (mode == TransmissionMode::Protected && !g.authorized.load()) {
            const auto& protectedFrame = g.additionalConnectionLock.load() ? extraConnectionFrame : waitingFrame;
            if (!sender.sendFrame(protectedFrame.data(), placeholderWidth, placeholderHeight, fps)) break;
        } else {
            if (!captureToBuffer(source, showCursor, frame, width, height)) {
                postStatus(L"Falha de captura");
                break;
            }
            if (!sender.sendFrame(frame.data(), width, height, fps)) {
                postStatus(L"Falha no envio NDI");
                break;
            }
        }

        nextFrame += std::chrono::microseconds(1000000 / fps);
        std::this_thread::sleep_until(nextFrame);
        if (std::chrono::steady_clock::now() - nextFrame > 1s) nextFrame = std::chrono::steady_clock::now();
    }

    sender.close();
    g.running = false;
    g.stopRequested = false;
    g.authorized = false;
    g.additionalConnectionLock = false;
    g.privacyBlocked = false;
    g.rawConnections = 0;
    g.baselineConnections = 0;
    postConnectionState(0);
    postStatus(L"Pronta para iniciar");
    PostMessageW(g.window, kWorkerEnded, 0, 0);
}

void startTransmission() {
    if (g.running.load() || g.stopRequested.load()) return;
    const std::wstring sourceName = getText(g.sourceName);
    if (sourceName.empty()) {
        MessageBoxW(g.window, L"Informe um nome para a fonte NDI.", L"Transmissor NDI", MB_OK | MB_ICONWARNING);
        return;
    }
    CaptureSource source = currentSource();
    if (source.bounds.right <= source.bounds.left || source.bounds.bottom <= source.bounds.top) {
        MessageBoxW(g.window, L"Selecione uma fonte de captura válida.", L"Transmissor NDI", MB_OK | MB_ICONWARNING);
        return;
    }

    collectSettings();
    const int fps = g.settings.fps;
    const bool showCursor = g.settings.showCursor;
    const TransmissionMode mode = g.settings.mode;
    g.activeProtectedMode = mode == TransmissionMode::Protected;
    traySetImageHidden(false);
    g.authorized = mode == TransmissionMode::Quick;
    g.additionalConnectionLock = false;
    g.privacyBlocked = false;
    g.rawConnections = 0;
    g.baselineConnections = 0;
    g.stopRequested = false;
    g.running = true;
    g.closeAfterWorker = false;
    updateControls();
    InvalidateRect(g.window, nullptr, FALSE);

    std::lock_guard<std::mutex> lock(g.workerMutex);
    if (g.worker.joinable()) g.worker.join();
    g.worker = std::thread(transmissionWorker, source, sourceName, utf8(sourceName), fps, showCursor, mode);
}

void releaseTransmission() {
    if (!g.running.load() || g.stopRequested.load() || !g.activeProtectedMode) return;
    const int raw = g.rawConnections.load();
    const int baseline = g.baselineConnections.load();
    if (raw <= 0 || (baseline > 0 && raw > baseline)) return;
    if (baseline == 0) g.baselineConnections = raw;
    g.additionalConnectionLock = false;
    g.authorized = true;
    postStatus(g.privacyBlocked.load() ? L"Liberada · privacidade ativa" : L"Transmissão liberada");
    updateControls();
    InvalidateRect(g.window, nullptr, FALSE);
}

void stopTransmission() {
    if (!g.running.load() || g.stopRequested.load()) return;
    g.stopRequested = true;
    postStatus(L"Parando transmissão...");
    updateControls();
    InvalidateRect(g.window, nullptr, FALSE);
}

void showHelp() {
    MessageBoxW(g.window,
        L"1. Escolha Monitor ou Janela e confirme a fonte na prévia.\n"
        L"2. Desmarque Enviar áudio se quiser transmitir somente a imagem.\n"
        L"3. No Modo protegido, libere o receptor quando ele for detectado.\n"
        L"4. Um receptor NDI pode abrir mais de uma conexão técnica; o app memoriza a linha de base.\n"
        L"5. Se as conexões subirem acima da linha de base, a imagem é bloqueada.\n"
        L"6. Todas as versões do WhatsApp e do Telegram são ocultadas automaticamente.\n"
        L"7. Marque a permissão correspondente para exibir um deles temporariamente.\n"
        L"8. Use Ocultar imagem para bloquear manualmente a imagem e o áudio sem desconectar a fonte.\n\n"
        L"O botão principal alterna entre Iniciar transmissão e Parar transmissão.",
        L"Como usar — V3", MB_OK | MB_ICONINFORMATION);
}

HWND addControl(const wchar_t* cls, const wchar_t* text, DWORD style,
                int x, int y, int w, int h, int id, HFONT font = nullptr) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g.window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (font) setControlFont(hwnd, font);
    applyDarkTheme(hwnd);
    return hwnd;
}

void drawPanel(HDC dc, RECT r) {
    HBRUSH brush = CreateSolidBrush(kPanel);
    HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, 14, 14);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void drawSectionTitle(HDC dc, int x, int y, const wchar_t* text) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kText);
    HFONT old = static_cast<HFONT>(SelectObject(dc, gFontBold));
    RECT r{x, y, x + 330, y + 26};
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void drawSmallText(HDC dc, int x, int y, const wchar_t* text, int width = 300) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kMuted);
    HFONT old = static_cast<HFONT>(SelectObject(dc, gFontSmall));
    RECT r{x, y, x + width, y + 22};
    DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void drawAudioMeter(HDC dc) {
    drawSmallText(dc, 426, 318, L"Nível", 70);
    const float level = ndiAudioOutputLevel();
    constexpr int segments = 10;
    constexpr int segmentWidth = 5;
    constexpr int gap = 2;
    const int lit = static_cast<int>(level * segments + 0.5f);
    for (int i = 0; i < segments; ++i) {
        const int x = 426 + i * (segmentWidth + gap);
        RECT bar{x, 346, x + segmentWidth, 364};
        COLORREF color = RGB(42, 55, 72);
        if (i < lit) color = i < 7 ? RGB(47, 210, 111) : RGB(248, 190, 55);
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &bar, brush);
        DeleteObject(brush);
    }
}

void createUi() {
    g.help = addControl(L"BUTTON", L"?  Como usar", BS_PUSHBUTTON, 806, 20, 116, 38, IdHelp, gFont);
    g.start = addControl(L"BUTTON", L"Iniciar transmissão", BS_DEFPUSHBUTTON, 486, 18, 180, 42, IdStart, gFontBold);
    g.sourceName = addControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 32, 208, 466, 34, IdSourceName, gFont);
    g.captureKind = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 32, 274, 140, 220, IdCaptureKind, gFont);
    SendMessageW(g.captureKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Monitor"));
    SendMessageW(g.captureKind, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Janela"));
    g.captureSource = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 182, 274, 266, 260, IdCaptureSource, gFont);
    g.refresh = addControl(L"BUTTON", L"↻", BS_PUSHBUTTON, 458, 274, 40, 34, IdRefresh, gFontBold);
    g.fps = addControl(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST, 32, 340, 120, 120, IdFps, gFont);
    SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 FPS"));
    SendMessageW(g.fps, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 FPS"));
    g.cursor = addControl(L"BUTTON", L"Mostrar cursor", BS_AUTOCHECKBOX, 166, 342, 150, 28, IdCursor, gFont);
    g.protectedMode = addControl(L"BUTTON", L"Modo protegido",
        BS_AUTORADIOBUTTON | WS_GROUP, 32, 402, 218, 56, IdProtected, gFont);
    g.quickMode = addControl(L"BUTTON", L"Modo rápido",
        BS_AUTORADIOBUTTON, 260, 402, 238, 56, IdQuick, gFont);
    g.allowWhatsApp = addControl(L"BUTTON", L"Permitir envio do WhatsApp", BS_AUTOCHECKBOX,
        32, 516, 226, 30, IdAllowWhatsApp, gFont);
    g.allowTelegram = addControl(L"BUTTON", L"Permitir envio do Telegram", BS_AUTOCHECKBOX,
        270, 516, 228, 30, IdAllowTelegram, gFont);

    g.release = addControl(L"BUTTON", L"Liberar transmissão", BS_PUSHBUTTON, 540, 312, 366, 38, IdRelease, gFontBold);
    g.manualPrivacy = addControl(L"BUTTON", L"Ocultar imagem", BS_PUSHBUTTON, 754, 370, 144, 38, IdManualPrivacy, gFontBold);
    g.preview = CreateWindowExW(0, kPreviewClass, L"", WS_CHILD | WS_VISIBLE, 548, 418, 350, 132,
        g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IdPreview)), GetModuleHandleW(nullptr), nullptr);
    g.receivers = addControl(L"STATIC", L"Nenhum receptor", SS_LEFT | SS_ENDELLIPSIS, 252, 100, 190, 22, IdReceivers, gFontBold);
    g.status = addControl(L"STATIC", L"Pronta para iniciar", SS_LEFT | SS_ENDELLIPSIS, 32, 100, 205, 22, IdStatus, gFontBold);
}

LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        drawPreview(dc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.window = hwnd;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        g.settings = loadSettings(iniPath());
        createUi();
        SetWindowTextW(g.sourceName, g.settings.sourceName.c_str());
        SendMessageW(g.captureKind, CB_SETCURSEL, g.settings.captureKind == CaptureKind::Window ? 1 : 0, 0);
        refreshSources(g.settings.sourceIndex);
        SendMessageW(g.fps, CB_SETCURSEL, g.settings.fps == 60 ? 1 : 0, 0);
        setChecked(g.cursor, g.settings.showCursor);
        setChecked(g.settings.mode == TransmissionMode::Quick ? g.quickMode : g.protectedMode, true);
        setChecked(g.allowWhatsApp, false);
        setChecked(g.allowTelegram, false);
        g.allowWa = false;
        g.allowTg = false;
        updateControls();
        SetTimer(hwnd, kPreviewTimer, 250, nullptr);
        if (g.settings.showHelpOnStart) PostMessageW(hwnd, WM_COMMAND, IdHelp, 0);
        return 0;
    }

    case WM_TIMER:
        if (wp == kPreviewTimer) {
            updatePreview();
            RECT meter{424, 338, 498, 370};
            InvalidateRect(hwnd, &meter, FALSE);
            if (g.manualPrivacy) {
                SetWindowTextW(g.manualPrivacy, trayImageHidden() ? L"Mostrar imagem" : L"Ocultar imagem");
                InvalidateRect(g.manualPrivacy, nullptr, FALSE);
            }
        }
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IdCaptureKind && HIWORD(wp) == CBN_SELCHANGE) refreshSources();
        else if (id == IdCaptureSource && HIWORD(wp) == CBN_SELCHANGE) updatePreview();
        else if (id == IdRefresh) refreshSources();
        else if (id == IdStart) {
            if (g.running.load()) stopTransmission(); else startTransmission();
        } else if (id == IdRelease) releaseTransmission();
        else if (id == IdManualPrivacy && g.running.load()) {
            const bool hidden = !trayImageHidden();
            traySetImageHidden(hidden);
            g.privacyBlocked = hidden;
            postStatus(hidden ? L"Privacidade manual · imagem e áudio ocultos" : L"Privacidade manual desativada");
            updatePreview();
            updateControls();
        }
        else if (id == IdAllowWhatsApp) {
            g.allowWa = isChecked(g.allowWhatsApp);
            postStatus(g.allowWa.load() ? L"WhatsApp permitido nesta execução" : L"Proteção do WhatsApp ativada");
            updatePreview();
        } else if (id == IdAllowTelegram) {
            g.allowTg = isChecked(g.allowTelegram);
            postStatus(g.allowTg.load() ? L"Telegram permitido nesta execução" : L"Proteção do Telegram ativada");
            updatePreview();
        } else if (id == IdHelp) showHelp();
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel);
        return reinterpret_cast<INT_PTR>(gPanelBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, kPanel2);
        return reinterpret_cast<INT_PTR>(gEditBrush);
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, gBgBrush);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kText);
        HFONT old = static_cast<HFONT>(SelectObject(dc, gFontTitle));
        RECT title{32, 18, 520, 48};
        DrawTextW(dc, L"Transmissor NDI Portátil", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, gFontSmall);
        SetTextColor(dc, kMuted);
        RECT sub{32, 48, 420, 68};
        DrawTextW(dc, L"Windows · versão de testes", -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        drawPanel(dc, {20, 78, 924, 138});
        drawSmallText(dc, 32, 84, L"Transmissão", 180);
        drawSmallText(dc, 252, 84, L"Receptor", 170);
        drawSmallText(dc, 474, 84, L"Envio", 150);
        drawSmallText(dc, 700, 84, L"Desempenho", 180);
        SetTextColor(dc, kText);
        SelectObject(dc, gFontBold);
        RECT envio{474, 100, 660, 127};
        DrawTextW(dc, L"— Mbps", -1, &envio, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT perf{700, 100, 900, 127};
        const wchar_t* perfText = g.stopRequested.load() ? L"Encerrando" :
            (g.privacyBlocked.load() ? L"Privacidade ativa" : (g.running.load() ? L"Em transmissão" : L"Aguardando"));
        DrawTextW(dc, perfText, -1, &perf, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        drawPanel(dc, {20, 150, 510, 640});
        drawPanel(dc, {522, 150, 924, 640});
        drawSectionTitle(dc, 32, 164, L"Configuração da transmissão");
        drawSmallText(dc, 32, 188, L"Nome da fonte NDI", 250);
        drawSmallText(dc, 32, 252, L"O que deseja transmitir", 260);
        drawSmallText(dc, 32, 318, L"Qualidade", 150);
        drawSmallText(dc, 32, 380, L"Modo de transmissão", 220);
        drawAudioMeter(dc);
        drawSectionTitle(dc, 32, 478, L"Permitir envio durante esta execução");
        drawSmallText(dc, 32, 554, L"As permissões não são salvas e reiniciam protegidas.", 450);

        drawSectionTitle(dc, 540, 164, L"Controle de acesso");
        SetTextColor(dc, kText);
        SelectObject(dc, gFontTitle);
        RECT number{540, 208, 906, 246};
        DrawTextW(dc, g.rawConnections.load() == 0 ? L"0" : L"1", -1, &number, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, gFontSmall);
        SetTextColor(dc, kMuted);
        RECT access{550, 252, 896, 304};
        const wchar_t* accessText = g.rawConnections.load() == 0
            ? L"Nenhum receptor conectado. A imagem ficará em espera até sua confirmação."
            : g.additionalConnectionLock.load()
                ? L"Conexão adicional detectada. A transmissão permanece bloqueada."
                : g.authorized.load()
                    ? L"Receptor autorizado. A transmissão está liberada."
                    : L"Receptor detectado. Clique em Liberar transmissão.";
        DrawTextW(dc, accessText, -1, &access, DT_CENTER | DT_WORDBREAK);

        RECT privacy{534, 360, 912, 562};
        HBRUSH privacyBrush = CreateSolidBrush(g.privacyBlocked.load() ? RGB(65, 43, 31) : RGB(15, 55, 43));
        HPEN privacyPen = CreatePen(PS_SOLID, 1, g.privacyBlocked.load() ? RGB(181, 112, 45) : RGB(37, 145, 88));
        HGDIOBJ oldPrivacyBrush = SelectObject(dc, privacyBrush);
        HGDIOBJ oldPrivacyPen = SelectObject(dc, privacyPen);
        RoundRect(dc, privacy.left, privacy.top, privacy.right, privacy.bottom, 14, 14);
        SelectObject(dc, oldPrivacyBrush);
        SelectObject(dc, oldPrivacyPen);
        DeleteObject(privacyBrush);
        DeleteObject(privacyPen);
        SetTextColor(dc, g.privacyBlocked.load() ? RGB(255, 218, 176) : RGB(197, 246, 218));
        SelectObject(dc, gFontBold);
        RECT p1{550, 370, 744, 392};
        const wchar_t* privacyTitle = trayImageHidden() ? L"Imagem ocultada" :
            (g.privacyBlocked.load() ? L"Conteúdo privado bloqueado" : L"Modo Privacidade ativo");
        DrawTextW(dc, privacyTitle, -1, &p1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, gFontSmall);
        RECT p2{550, 390, 744, 412};
        DrawTextW(dc, trayImageHidden() ? L"Imagem e áudio bloqueados." : L"WhatsApp e Telegram protegidos.", -1, &p2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        drawPanel(dc, {534, 572, 912, 628});
        SelectObject(dc, old);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case kStatusMessage: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        SetWindowTextW(g.status, text->c_str());
        updateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case kReceiverMessage: {
        const int raw = static_cast<int>(wp);
        if (raw == 0) SetWindowTextW(g.receivers, L"Nenhum receptor");
        else if (g.additionalConnectionLock.load()) SetWindowTextW(g.receivers, L"Conexão adicional");
        else if (g.authorized.load()) SetWindowTextW(g.receivers, L"Receptor autorizado");
        else SetWindowTextW(g.receivers, L"Receptor detectado");
        if (g.activeProtectedMode && g.additionalConnectionLock.load()) {
            FLASHWINFO flash{sizeof(flash), hwnd, FLASHW_TRAY, 1, 0};
            FlashWindowEx(&flash);
        }
        updateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case kWorkerEnded: {
        if (g.worker.joinable()) {
            std::lock_guard<std::mutex> lock(g.workerMutex);
            if (g.worker.joinable()) g.worker.join();
        }
        updateControls();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (g.closeAfterWorker) {
            g.closeAfterWorker = false;
            collectSettings();
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_CLOSE:
        if (g.running.load()) {
            if (MessageBoxW(hwnd, L"Há uma transmissão ativa. Deseja parar e sair?", L"Transmissão ativa", MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            g.closeAfterWorker = true;
            stopTransmission();
            return 0;
        }
        collectSettings();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kPreviewTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    gBgBrush = CreateSolidBrush(kBg);
    gPanelBrush = CreateSolidBrush(kPanel);
    gPanel2Brush = CreateSolidBrush(kPanel2);
    gEditBrush = CreateSolidBrush(kPanel2);

    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfHeight = -16; gFont = CreateFontIndirectW(&lf);
    lf.lfHeight = -14; gFontSmall = CreateFontIndirectW(&lf);
    lf.lfHeight = -16; lf.lfWeight = FW_SEMIBOLD; gFontBold = CreateFontIndirectW(&lf);
    lf.lfHeight = -23; gFontTitle = CreateFontIndirectW(&lf);

    WNDCLASSEXW previewClass{sizeof(previewClass)};
    previewClass.lpfnWndProc = previewProc;
    previewClass.hInstance = instance;
    previewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    previewClass.hbrBackground = gPanel2Brush;
    previewClass.lpszClassName = kPreviewClass;
    if (!RegisterClassExW(&previewClass)) return 1;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = gBgBrush;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"Transmissor NDI Portátil — V3",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 690, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g.worker.joinable()) {
        g.stopRequested = true;
        g.worker.join();
    }
    DeleteObject(gFont);
    DeleteObject(gFontSmall);
    DeleteObject(gFontBold);
    DeleteObject(gFontTitle);
    DeleteObject(gBgBrush);
    DeleteObject(gPanelBrush);
    DeleteObject(gPanel2Brush);
    DeleteObject(gEditBrush);
    return static_cast<int>(msg.wParam);
}
