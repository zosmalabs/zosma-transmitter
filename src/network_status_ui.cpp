#include <windows.h>
#include <commctrl.h>
#include <iphlpapi.h>
#include <wlanapi.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "protected_ip_ui.h"

namespace {
constexpr wchar_t kMainWindowClass[] = L"TransmissorNDIPortatilV3";
constexpr UINT_PTR kNetworkTimer = 91;
constexpr int kIdFps = 1005;
constexpr int kIdStart = 1011;
constexpr int kTopSendId = 1401;
constexpr int kTopPerformanceId = 1402;
constexpr int kFooterPrimaryId = 1403;
constexpr int kFooterSecondaryId = 1404;

HWND gMain{};
HWND gTopSend{};
HWND gTopPerformance{};
HWND gFooterPrimary{};
HWND gFooterSecondary{};
HHOOK gHook{};

struct SampleState {
    DWORD interfaceIndex{};
    bool haveBaseline{false};
    DWORD previousOutOctets{};
    std::chrono::steady_clock::time_point previousTime{};
};
SampleState gSample;

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
    return start && controlText(start).find(L"Parar transmissão") != std::wstring::npos;
}

int selectedFps() {
    HWND fps = gMain ? GetDlgItem(gMain, kIdFps) : nullptr;
    if (!fps) return 30;
    return SendMessageW(fps, CB_GETCURSEL, 0, 0) == 1 ? 60 : 30;
}

int wifiSignalQuality(DWORD interfaceIndex) {
    MIB_IFROW ifRow{};
    ifRow.dwIndex = interfaceIndex;
    if (GetIfEntry(&ifRow) != NO_ERROR) return -1;

    HANDLE client = nullptr;
    DWORD negotiated = 0;
    if (WlanOpenHandle(2, nullptr, &negotiated, &client) != ERROR_SUCCESS || !client) return -1;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    int quality = -1;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS && interfaces) {
        // Em máquinas com uma única interface Wi-Fi ativa, usamos a conexão WLAN conectada.
        // Evitamos conversões LUID/GUID que variam entre versões do Windows SDK do runner.
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const auto& item = interfaces->InterfaceInfo[i];
            if (item.isState != wlan_interface_state_connected) continue;
            DWORD size = 0;
            WLAN_OPCODE_VALUE_TYPE opcode{};
            PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
            if (WlanQueryInterface(client, &item.InterfaceGuid, wlan_intf_opcode_current_connection,
                                   nullptr, &size, reinterpret_cast<PVOID*>(&attrs), &opcode) == ERROR_SUCCESS && attrs) {
                quality = static_cast<int>(attrs->wlanAssociationAttributes.wlanSignalQuality);
                WlanFreeMemory(attrs);
                break;
            }
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(client, nullptr);
    return quality;
}

bool preferredInterfaceIndex(ULONG& index) {
    SOCKADDR_IN target{};
    target.sin_family = AF_INET;
    const std::string trusted = configuredReceiverIp();
    if (!trusted.empty() && InetPtonA(AF_INET, trusted.c_str(), &target.sin_addr) == 1) {
        // usa o receptor autorizado como destino para escolher a rota
    } else {
        target.sin_addr.s_addr = htonl(0x08080808);
    }
    index = 0;
    return GetBestInterfaceEx(reinterpret_cast<SOCKADDR*>(&target), &index) == NO_ERROR && index != 0;
}

bool chooseActiveInterface(MIB_IFROW& row, bool& isWifi) {
    ULONG size = 0;
    if (GetIfTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    std::vector<unsigned char> storage(size);
    auto* table = reinterpret_cast<PMIB_IFTABLE>(storage.data());
    if (GetIfTable(table, &size, FALSE) != NO_ERROR) return false;

    ULONG preferred = 0;
    preferredInterfaceIndex(preferred);
    const MIB_IFROW* chosen = nullptr;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& candidate = table->table[i];
        if (candidate.dwIndex == preferred && candidate.dwOperStatus == IF_OPER_STATUS_OPERATIONAL) {
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) {
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& candidate = table->table[i];
            if (candidate.dwOperStatus != IF_OPER_STATUS_OPERATIONAL) continue;
            if (candidate.dwType == IF_TYPE_SOFTWARE_LOOPBACK || candidate.dwType == IF_TYPE_TUNNEL) continue;
            chosen = &candidate;
            break;
        }
    }
    if (!chosen) return false;
    row = *chosen;
    isWifi = row.dwType == IF_TYPE_IEEE80211;
    return true;
}

std::wstring formatRate(double mbps) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(mbps < 10.0 ? 1 : 0) << mbps << L" Mbps";
    return out.str();
}

std::wstring formatLink(std::uint64_t bitsPerSecond) {
    const double mbps = static_cast<double>(bitsPerSecond) / 1'000'000.0;
    if (mbps >= 1000.0) {
        std::wostringstream out;
        out << std::fixed << std::setprecision(mbps >= 10'000.0 ? 0 : 1) << mbps / 1000.0 << L" Gbps";
        return out.str();
    }
    std::wostringstream out;
    out << std::fixed << std::setprecision(0) << mbps << L" Mbps";
    return out.str();
}

std::wstring stabilityState(bool wifi, int signal, double linkMbps, double outgoingMbps) {
    if (linkMbps <= 0.0) return L"Sem rede";
    const double utilization = outgoingMbps / linkMbps;
    if ((wifi && signal >= 0 && signal < 35) || linkMbps < 40.0 || utilization > 0.90) return L"Instável";
    if ((wifi && signal >= 0 && signal < 60) || linkMbps < 100.0 || utilization > 0.70) return L"Atenção";
    return L"Estável";
}

void setLabel(HWND hwnd, const std::wstring& text) { if (hwnd) SetWindowTextW(hwnd, text.c_str()); }

void updateNetworkStatus() {
    if (!gMain) return;
    MIB_IFROW row{};
    bool wifi = false;
    if (!chooseActiveInterface(row, wifi)) {
        gSample = {};
        setLabel(gTopSend, L"— Mbps");
        setLabel(gTopPerformance, L"Sem rede");
        setLabel(gFooterPrimary, L"Sem rede · " + std::to_wstring(selectedFps()) + L" fps · Sem rede");
        setLabel(gFooterSecondary, L"Nenhuma interface de rede ativa foi encontrada.");
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    double outgoingMbps = 0.0;
    if (gSample.haveBaseline && gSample.interfaceIndex == row.dwIndex) {
        const double seconds = std::chrono::duration<double>(now - gSample.previousTime).count();
        const DWORD delta = row.dwOutOctets - gSample.previousOutOctets;
        if (seconds > 0.05) outgoingMbps = static_cast<double>(delta) * 8.0 / seconds / 1'000'000.0;
    }
    gSample.interfaceIndex = row.dwIndex;
    gSample.previousOutOctets = row.dwOutOctets;
    gSample.previousTime = now;
    gSample.haveBaseline = true;

    const std::uint64_t linkSpeed = static_cast<std::uint64_t>(row.dwSpeed);
    const double linkMbps = static_cast<double>(linkSpeed) / 1'000'000.0;
    const int signal = wifi ? wifiSignalQuality(row.dwIndex) : -1;
    const std::wstring state = stabilityState(wifi, signal, linkMbps, outgoingMbps);

    std::wstring networkName = wifi ? L"Wi-Fi" : L"Ethernet";
    if (wifi && signal >= 0) networkName += L" " + std::to_wstring(signal) + L"%";
    const std::wstring primary = networkName + L" · " + formatLink(linkSpeed) + L" · " +
                                 std::to_wstring(selectedFps()) + L" fps · " + state;
    const std::wstring secondary = transmissionRunning()
        ? L"Envio pela interface ativa: " + formatRate(outgoingMbps)
        : L"Rede pronta · a taxa de envio aparece durante a transmissão.";
    setLabel(gTopSend, transmissionRunning() ? formatRate(outgoingMbps) : L"— Mbps");
    setLabel(gTopPerformance, transmissionRunning() ? state : L"Aguardando");
    setLabel(gFooterPrimary, primary);
    setLabel(gFooterSecondary, secondary);
}

HWND addStatic(HWND parent, int id, int x, int y, int w, int h, HFONT font) {
    HWND label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (font) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return label;
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_TIMER && wp == kNetworkTimer) { updateNetworkStatus(); return 0; }
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == kIdFps || id == kIdStart) updateNetworkStatus();
    } else if (msg == WM_NCDESTROY) {
        KillTimer(hwnd, kNetworkTimer);
        RemoveWindowSubclass(hwnd, subclassProc, 3);
        gMain = gTopSend = gTopPerformance = gFooterPrimary = gFooterSecondary = nullptr;
        gSample = {};
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installUi(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;
    HFONT normal = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (HWND fps = GetDlgItem(hwnd, kIdFps)) {
        if (HFONT candidate = reinterpret_cast<HFONT>(SendMessageW(fps, WM_GETFONT, 0, 0))) normal = candidate;
    }
    gTopSend = addStatic(hwnd, kTopSendId, 474, 100, 186, 27, normal);
    gTopPerformance = addStatic(hwnd, kTopPerformanceId, 700, 100, 200, 27, normal);
    gFooterPrimary = addStatic(hwnd, kFooterPrimaryId, 548, 580, 350, 22, normal);
    gFooterSecondary = addStatic(hwnd, kFooterSecondaryId, 548, 604, 350, 20, normal);
    SetWindowSubclass(hwnd, subclassProc, 3, 0);
    SetTimer(hwnd, kNetworkTimer, 1000, nullptr);
    updateNetworkStatus();
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, kMainWindowClass) == 0) installUi(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct NetworkBootstrap {
    NetworkBootstrap() { gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId()); }
    ~NetworkBootstrap() { if (gHook) UnhookWindowsHookEx(gHook); }
} gBootstrap;
}
