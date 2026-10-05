#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <iterator>
#include <string>

namespace {
constexpr wchar_t kMainClass[] = L"TransmissorNDIPortatilV3";
constexpr wchar_t kAboutClass[] = L"ZosmaTransmitterAboutStableV1";
constexpr UINT_PTR kOneShotStyleTimer = 193;

constexpr int kIdProtected = 1007;
constexpr int kIdQuick = 1008;
constexpr int kIdStart = 1011;
constexpr int kIdRelease = 1012;
constexpr int kIdHelp = 1015;
constexpr int kIdManualPrivacy = 1017;
constexpr int kIdAbout = 1501;
constexpr int kIdSite = 1502;
constexpr int kIdLicenses = 1503;
constexpr int kIdClose = 1504;

constexpr COLORREF kBg = RGB(12, 18, 28);
constexpr COLORREF kPanel = RGB(20, 31, 47);
constexpr COLORREF kPanelHot = RGB(28, 45, 67);
constexpr COLORREF kBorder = RGB(58, 82, 112);
constexpr COLORREF kAccent = RGB(38, 151, 255);
constexpr COLORREF kText = RGB(242, 247, 255);
constexpr COLORREF kMuted = RGB(159, 181, 211);
constexpr COLORREF kDisabled = RGB(105, 119, 137);
constexpr UINT_PTR kComboSubclass = 172;

HWND gMain{};
HWND gAbout{};
HHOOK gHook{};

HFONT makeFont(int px, int weight) {
    LOGFONTW lf{};
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    lf.lfHeight = -px;
    lf.lfWeight = weight;
    return CreateFontIndirectW(&lf);
}

bool mouseInside(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(hwnd, &pt);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    return PtInRect(&rc, pt) != FALSE;
}

void roundControl(HWND hwnd, int radius) {
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc) || rc.right <= 0 || rc.bottom <= 0) return;
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, radius, radius);
    SetWindowRgn(hwnd, rgn, TRUE);
}

void paintCombo(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool focused = GetFocus() == hwnd;

    HBRUSH brush = CreateSolidBrush(RGB(17, 27, 40));
    HPEN pen = CreatePen(PS_SOLID, focused ? 2 : 1, focused ? kAccent : kBorder);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[512]{};
    const int selected = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    if (selected >= 0) SendMessageW(hwnd, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kText : kDisabled);
    RECT tr{12, 0, rc.right - 38, rc.bottom};
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    HPEN arrowPen = CreatePen(PS_SOLID, 2, enabled ? RGB(201, 220, 244) : kDisabled);
    HGDIOBJ oldArrow = SelectObject(dc, arrowPen);
    const int cx = rc.right - 19;
    const int cy = rc.bottom / 2;
    MoveToEx(dc, cx - 5, cy - 2, nullptr);
    LineTo(dc, cx, cy + 3);
    LineTo(dc, cx + 5, cy - 2);
    SelectObject(dc, oldArrow);
    DeleteObject(arrowPen);
    if (oldFont) SelectObject(dc, oldFont);
}

LRESULT CALLBACK comboProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        paintCombo(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_NCPAINT) return 0;
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_ENABLE ||
        msg == CB_SETCURSEL || msg == WM_LBUTTONUP || msg == WM_KEYUP) {
        const LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, comboProc, kComboSubclass);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void paintPushButton(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int id = GetDlgCtrlID(hwnd);
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool hot = enabled && mouseInside(hwnd);
    const bool down = (SendMessageW(hwnd, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;

    COLORREF fill = kPanel;
    COLORREF border = kBorder;
    if (id == kIdStart) {
        fill = enabled ? (down ? RGB(19, 109, 204) : hot ? RGB(54, 169, 255) : kAccent) : RGB(46, 62, 80);
        border = enabled ? RGB(94, 190, 255) : RGB(71, 82, 96);
    } else if (id == kIdRelease) {
        fill = enabled ? (hot ? RGB(48, 65, 86) : RGB(39, 52, 69)) : RGB(48, 53, 61);
    } else if (id == kIdManualPrivacy) {
        fill = enabled ? (down ? RGB(126, 67, 9) : hot ? RGB(180, 96, 15) : RGB(151, 78, 10)) : RGB(67, 60, 52);
        border = enabled ? RGB(232, 139, 43) : RGB(94, 83, 70);
    } else if (hot) {
        fill = kPanelHot;
        border = RGB(84, 117, 155);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 12, 12);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t text[256]{};
    GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kText : kDisabled);
    RECT textRect = rc;
    if (id == kIdStart) {
        const int cy = rc.bottom / 2;
        HPEN iconPen = CreatePen(PS_SOLID, 2, enabled ? RGB(246, 251, 255) : kDisabled);
        HGDIOBJ oldIconPen = SelectObject(dc, iconPen);
        HBRUSH iconBrush = CreateSolidBrush(enabled ? RGB(246, 251, 255) : kDisabled);
        HGDIOBJ oldIconBrush = SelectObject(dc, iconBrush);
        Ellipse(dc, 18, cy - 3, 24, cy + 3);
        SelectObject(dc, oldIconBrush);
        DeleteObject(iconBrush);
        Arc(dc, 12, cy - 9, 30, cy + 9, 25, cy - 7, 25, cy + 7);
        Arc(dc, 7, cy - 14, 35, cy + 14, 27, cy - 11, 27, cy + 11);
        SelectObject(dc, oldIconPen);
        DeleteObject(iconPen);
        textRect.left = 36;
        textRect.right -= 2;
    }
    DrawTextW(dc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
}

void paintCheck(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(kPanel);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    RECT box{2, (rc.bottom - 18) / 2, 20, (rc.bottom - 18) / 2 + 18};
    HBRUSH brush = CreateSolidBrush(checked ? kAccent : RGB(17, 27, 40));
    HPEN pen = CreatePen(PS_SOLID, 1, checked ? RGB(96, 193, 255) : RGB(82, 106, 136));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, 6, 6);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    if (checked) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, RGB(244, 250, 255));
        HGDIOBJ old = SelectObject(dc, checkPen);
        MoveToEx(dc, 6, box.top + 9, nullptr);
        LineTo(dc, 10, box.top + 13);
        LineTo(dc, 17, box.top + 5);
        SelectObject(dc, old);
        DeleteObject(checkPen);
    }

    wchar_t text[256]{};
    GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? kText : kDisabled);
    RECT tr{28, 0, rc.right, rc.bottom};
    DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
}

void paintRadioCard(HWND hwnd, HDC dc) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool enabled = IsWindowEnabled(hwnd) != FALSE;
    const bool hot = enabled && mouseInside(hwnd);

    HBRUSH brush = CreateSolidBrush(checked ? RGB(15, 47, 79) : hot ? RGB(24, 39, 58) : RGB(18, 29, 43));
    HPEN pen = CreatePen(PS_SOLID, checked ? 2 : 1, checked ? kAccent : hot ? RGB(78, 107, 143) : kBorder);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 14, 14);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    wchar_t buffer[256]{};
    GetWindowTextW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    std::wstring label = buffer;
    const size_t split = label.find(L"\r\n");
    const std::wstring title = split == std::wstring::npos ? label : label.substr(0, split);
    HFONT titleFont = makeFont(15, FW_SEMIBOLD);
    HGDIOBJ oldFont = titleFont ? SelectObject(dc, titleFont) : nullptr;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? (checked ? RGB(220, 241, 255) : kText) : kDisabled);
    const wchar_t* symbol = GetDlgCtrlID(hwnd) == kIdProtected ? L"◆" : L"⚡";
    RECT iconRect{18, 0, 48, rc.bottom};
    DrawTextW(dc, symbol, -1, &iconRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT titleRect{48, 0, rc.right - 12, rc.bottom};
    DrawTextW(dc, title.c_str(), -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(dc, oldFont);
    if (titleFont) DeleteObject(titleFont);
}

LRESULT CALLBACK buttonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        const UINT type = static_cast<UINT>(GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK);
        if (type == BS_AUTORADIOBUTTON || type == BS_RADIOBUTTON) paintRadioCard(hwnd, dc);
        else if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX) paintCheck(hwnd, dc);
        else paintPushButton(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&track);
        InvalidateRect(hwnd, nullptr, FALSE);
    } else if (msg == WM_MOUSELEAVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
               msg == WM_ENABLE || msg == BM_SETCHECK || msg == WM_SETTEXT) {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        InvalidateRect(hwnd, nullptr, FALSE);
        return result;
    } else if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, buttonProc, 171);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void styleOne(HWND child) {
    if (!child || GetPropW(child, L"ZosmaStableStyled")) return;
    wchar_t cls[64]{};
    GetClassNameW(child, cls, static_cast<int>(std::size(cls)));
    if (_wcsicmp(cls, L"Button") == 0) {
        SetWindowSubclass(child, buttonProc, 171, 0);
        const UINT type = static_cast<UINT>(GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK);
        // Uma região arredondada no checkbox corta o quadrado localizado na borda esquerda.
        if (type != BS_AUTOCHECKBOX && type != BS_CHECKBOX) roundControl(child, 12);
        SetPropW(child, L"ZosmaStableStyled", reinterpret_cast<HANDLE>(1));
        InvalidateRect(child, nullptr, FALSE);
    } else if (_wcsicmp(cls, L"Edit") == 0) {
        roundControl(child, 10);
        SetPropW(child, L"ZosmaStableStyled", reinterpret_cast<HANDLE>(1));
    } else if (_wcsicmp(cls, WC_COMBOBOXW) == 0) {
        SetWindowTheme(child, L"", L"");
        SetWindowLongPtrW(child, GWL_STYLE, GetWindowLongPtrW(child, GWL_STYLE) & ~WS_BORDER);
        SetWindowSubclass(child, comboProc, kComboSubclass, 0);
        roundControl(child, 10);
        SetPropW(child, L"ZosmaStableStyled", reinterpret_cast<HANDLE>(1));
        InvalidateRect(child, nullptr, TRUE);
    }
}

BOOL CALLBACK enumProc(HWND child, LPARAM) {
    styleOne(child);
    return TRUE;
}

void styleControlsOnce() {
    if (gMain) EnumChildWindows(gMain, enumProc, 0);
}

void adjustCardsOnce() {
    if (!gMain) return;
    if (HWND card = GetDlgItem(gMain, kIdProtected))
        SetWindowPos(card, nullptr, 32, 402, 218, 56, SWP_NOZORDER | SWP_NOACTIVATE);
    if (HWND card = GetDlgItem(gMain, kIdQuick))
        SetWindowPos(card, nullptr, 260, 402, 238, 56, SWP_NOZORDER | SWP_NOACTIVATE);
}

void showLicenseNotice(HWND owner) {
    MessageBoxW(owner,
        L"Zosma Transmitter utiliza tecnologia NDI® para transmissão de vídeo e áudio em rede.\n\n"
        L"Zosma Transmitter é um software independente e não é afiliado, patrocinado, certificado ou desenvolvido pela Vizrt NDI AB.\n\n"
        L"NDI® é uma marca registrada da Vizrt NDI AB.\n\n"
        L"Os avisos aplicáveis aos componentes de terceiros acompanham o aplicativo no arquivo AVISOS-DE-TERCEIROS.txt.",
        L"Licenças e componentes de terceiros", MB_OK | MB_ICONINFORMATION);
}

LRESULT CALLBACK aboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        HFONT font = makeFont(15, FW_SEMIBOLD);
        HWND site = CreateWindowExW(0, L"BUTTON", L"zosma.com.br", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            28, 382, 170, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdSite)), GetModuleHandleW(nullptr), nullptr);
        HWND licenses = CreateWindowExW(0, L"BUTTON", L"Licenças e componentes", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            210, 382, 210, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdLicenses)), GetModuleHandleW(nullptr), nullptr);
        HWND close = CreateWindowExW(0, L"BUTTON", L"Fechar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            432, 382, 120, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdClose)), GetModuleHandleW(nullptr), nullptr);
        for (HWND button : {site, licenses, close}) {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SetWindowSubclass(button, buttonProc, 171, 0);
            roundControl(button, 12);
        }
        if (font) DeleteObject(font);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kIdSite) {
            ShellExecuteW(hwnd, L"open", L"https://zosma.com.br", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        if (LOWORD(wp) == kIdLicenses) { showLicenseNotice(hwnd); return 0; }
        if (LOWORD(wp) == kIdClose) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH bg = CreateSolidBrush(kBg);
        FillRect(dc, &client, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);

        HFONT title = makeFont(28, FW_SEMIBOLD);
        HFONT body = makeFont(15, FW_NORMAL);
        HFONT bold = makeFont(15, FW_SEMIBOLD);
        HFONT smallFont = makeFont(13, FW_NORMAL);
        HGDIOBJ oldFont = title ? SelectObject(dc, title) : nullptr;
        SetTextColor(dc, kText);
        RECT r{28, 24, 552, 58};
        DrawTextW(dc, L"Zosma Transmitter", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (body) SelectObject(dc, body);
        SetTextColor(dc, kMuted);
        r = {28, 60, 552, 84};
        DrawTextW(dc, L"Uma solução Zosma Labs  ·  Versão 0.3.5 Beta", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        HBRUSH panel = CreateSolidBrush(kPanel);
        HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ oldBrush = SelectObject(dc, panel);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, 28, 102, 552, 220, 16, 16);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(panel);
        DeleteObject(pen);

        if (bold) SelectObject(dc, bold);
        SetTextColor(dc, kText);
        r = {48, 120, 530, 146};
        DrawTextW(dc, L"Zosma Labs", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (body) SelectObject(dc, body);
        SetTextColor(dc, RGB(199, 220, 247));
        r = {48, 146, 530, 174};
        DrawTextW(dc, L"Ideias transformadas em software.", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (smallFont) SelectObject(dc, smallFont);
        SetTextColor(dc, kMuted);
        r = {48, 178, 530, 208};
        DrawTextW(dc, L"zosma.com.br", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (bold) SelectObject(dc, bold);
        SetTextColor(dc, kText);
        r = {28, 242, 552, 268};
        DrawTextW(dc, L"Tecnologia NDI®", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (smallFont) SelectObject(dc, smallFont);
        SetTextColor(dc, kMuted);
        r = {28, 270, 552, 352};
        DrawTextW(dc,
            L"Zosma Transmitter utiliza tecnologia NDI® para transmissão de vídeo e áudio em rede.\n\n"
            L"É um software independente e não é afiliado, patrocinado, certificado ou desenvolvido pela Vizrt NDI AB. NDI® é uma marca registrada da Vizrt NDI AB.",
            -1, &r, DT_LEFT | DT_WORDBREAK);

        if (oldFont) SelectObject(dc, oldFont);
        if (title) DeleteObject(title);
        if (body) DeleteObject(body);
        if (bold) DeleteObject(bold);
        if (smallFont) DeleteObject(smallFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: if (hwnd == gAbout) gAbout = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void showAbout() {
    if (gAbout && IsWindow(gAbout)) {
        ShowWindow(gAbout, SW_RESTORE);
        SetForegroundWindow(gAbout);
        return;
    }
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = aboutProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kAboutClass;
    RegisterClassExW(&wc);

    RECT owner{};
    GetWindowRect(gMain, &owner);
    const int width = 596;
    const int height = 486;
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;
    gAbout = CreateWindowExW(WS_EX_DLGMODALFRAME, kAboutClass, L"Sobre — Zosma Transmitter",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height, gMain, nullptr, instance, nullptr);
    if (gAbout) ShowWindow(gAbout, SW_SHOW);
}

LRESULT CALLBACK mainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND && LOWORD(wp) == kIdAbout) { showAbout(); return 0; }
    if (msg == WM_TIMER && wp == kOneShotStyleTimer) {
        KillTimer(hwnd, kOneShotStyleTimer);
        styleControlsOnce();
        adjustCardsOnce();
        return 0;
    }
    if (msg == WM_CTLCOLORLISTBOX) {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, kText);
        SetBkColor(dc, RGB(17, 27, 40));
        static HBRUSH listBrush = CreateSolidBrush(RGB(17, 27, 40));
        return reinterpret_cast<LRESULT>(listBrush);
    }
    if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kOneShotStyleTimer);
        RemoveWindowSubclass(hwnd, mainProc, 107);
        gMain = nullptr;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void install(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    SetWindowTextW(hwnd, L"Zosma Transmitter — V3");

    HFONT font = makeFont(15, FW_SEMIBOLD);
    HWND about = CreateWindowExW(0, L"BUTTON", L"Sobre", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        678, 20, 116, 38, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAbout)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(about, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    if (font) DeleteObject(font);

    SetWindowSubclass(hwnd, mainProc, 107, 0);
    if (HWND start = GetDlgItem(hwnd, kIdStart))
        SetWindowPos(start, nullptr, 456, 18, 210, 42, SWP_NOZORDER | SWP_NOACTIVATE);
    styleControlsOnce();
    adjustCardsOnce();
    // Uma única segunda passada pega controles auxiliares criados logo após o WM_CREATE.
    SetTimer(hwnd, kOneShotStyleTimer, 450, nullptr);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK hookProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainClass) == 0) install(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct Bootstrap {
    Bootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, hookProc, nullptr, GetCurrentThreadId()); }
    ~Bootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBootstrap;
}
