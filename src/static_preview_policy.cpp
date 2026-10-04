#include <windows.h>
#include <commctrl.h>

namespace {
constexpr wchar_t kMainClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT_PTR kPreviewTimer = 77;
constexpr int kIdCaptureKind = 1002;
constexpr int kIdCaptureSource = 1003;
constexpr int kIdRefresh = 1004;
constexpr int kIdPreview = 1016;
constexpr int kReferenceTitleId = 1301;
constexpr int kMonitorMessageId = 1302;

HWND gMain{};
HWND gReferenceTitle{};
HWND gMonitorMessage{};
HHOOK gHook{};
bool gAllowNextSnapshot{false};

bool windowMode() {
    HWND combo = gMain ? GetDlgItem(gMain, kIdCaptureKind) : nullptr;
    return combo && SendMessageW(combo, CB_GETCURSEL, 0, 0) == 1;
}

void refreshPreviewMode() {
    if (!gMain) return;
    HWND preview = GetDlgItem(gMain, kIdPreview);
    const bool window = windowMode();

    if (preview) ShowWindow(preview, window ? SW_SHOW : SW_HIDE);
    if (gMonitorMessage) ShowWindow(gMonitorMessage, window ? SW_HIDE : SW_SHOW);
    if (gReferenceTitle) {
        SetWindowTextW(gReferenceTitle, window ? L"Imagem de referência" : L"Captura selecionada");
        ShowWindow(gReferenceTitle, SW_SHOW);
    }
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kPreviewTimer) {
        refreshPreviewMode();
        if (!windowMode()) return 0;
        if (!gAllowNextSnapshot) return 0;
        gAllowNextSnapshot = false;
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == kIdCaptureKind && HIWORD(wp) == CBN_SELCHANGE) {
            // A lista de fontes será atualizada pelo wndProc principal. Permitimos
            // exatamente um frame no próximo timer para confirmar a janela escolhida.
            gAllowNextSnapshot = true;
            refreshPreviewMode();
        } else if (id == kIdCaptureSource && HIWORD(wp) == CBN_SELCHANGE) {
            // O wndProc principal já captura imediatamente ao trocar a janela.
            gAllowNextSnapshot = false;
            refreshPreviewMode();
        } else if (id == kIdRefresh) {
            gAllowNextSnapshot = true;
        }
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, subclassProc, 2);
        gMain = nullptr;
        gReferenceTitle = nullptr;
        gMonitorMessage = nullptr;
    }

    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installPolicy(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    // Cobre o título pintado pelo código antigo sem alterar a estrutura principal.
    gReferenceTitle = CreateWindowExW(0, L"STATIC", L"Imagem de referência",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        540, 424, 220, 20, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kReferenceTitleId)),
        GetModuleHandleW(nullptr), nullptr);

    // No modo Monitor não existe prévia: deixamos apenas uma indicação leve.
    gMonitorMessage = CreateWindowExW(0, L"STATIC",
        L"Monitor selecionado\r\n\r\nA prévia fica desativada para reduzir o uso de recursos.",
        WS_CHILD | SS_CENTER,
        540, 448, 366, 92, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMonitorMessageId)),
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(gReferenceTitle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gMonitorMessage, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    SetWindowSubclass(hwnd, subclassProc, 2, 0);
    gAllowNextSnapshot = windowMode();
    refreshPreviewMode();
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainClass) == 0) installPolicy(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct PreviewPolicyBootstrap {
    PreviewPolicyBootstrap() {
        gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId());
    }
    ~PreviewPolicyBootstrap() {
        if (gHook) UnhookWindowsHookEx(gHook);
    }
} gBootstrap;
}
