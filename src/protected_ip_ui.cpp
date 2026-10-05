#include "protected_ip_ui.h"
#include "ndi_sender.h"
#include "tray_controller.h"

#include <windows.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <propkeydef.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <uxtheme.h>
#include <ws2tcpip.h>

#include <atomic>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace {
constexpr int kIdProtected = 1007;
constexpr int kIdQuick = 1008;
constexpr int kIdStart = 1011;
constexpr int kIdRelease = 1012;
constexpr int kIdStatus = 1014;
constexpr int kIpEditId = 1201;
constexpr int kIpLabelId = 1202;
constexpr int kIpHintId = 1203;
constexpr int kAudioOptionId = 1210;
constexpr int kAudioOutputId = 1211;
constexpr int kAudioOutputLabelId = 1212;

HWND gMain{};
HWND gIpEdit{};
HWND gIpLabel{};
HWND gIpHint{};
HWND gAudioOption{};
HWND gAudioOutput{};
HWND gAudioOutputLabel{};
std::mutex gIpMutex;
std::mutex gAudioMutex;
std::string gConfiguredIp;
std::wstring gConfiguredAudioDeviceId;
std::atomic_bool gReleased{false};
std::atomic_bool gAudioRequested{true};
std::atomic_bool gAudioAllowed{false};
HHOOK gHook{};

struct AudioEndpoint {
    std::wstring id;
    std::wstring label;
};
std::vector<AudioEndpoint> gAudioEndpoints;

std::filesystem::path iniPath() {
    std::vector<wchar_t> buffer(32768);
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), len)).parent_path() / L"transmissor-ndi.ini";
}

std::wstring savedAudioDeviceId() {
    wchar_t value[2048]{};
    GetPrivateProfileStringW(L"app", L"audioOutputDeviceId", L"", value,
                             static_cast<DWORD>(std::size(value)), iniPath().c_str());
    return value;
}

void saveAudioDeviceId(const std::wstring& id) {
    WritePrivateProfileStringW(L"app", L"audioOutputDeviceId", id.c_str(), iniPath().c_str());
}

void refreshAudioEndpoints() {
    gAudioEndpoints.clear();
    gAudioEndpoints.push_back({L"", L"Padrão do Windows"});
    SendMessageW(gAudioOutput, CB_RESETCONTENT, 0, 0);
    SendMessageW(gAudioOutput, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(gAudioEndpoints.front().label.c_str()));

    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninitialize = SUCCEEDED(init);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator &&
        SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            IPropertyStore* properties = nullptr;
            LPWSTR id = nullptr;
            PROPVARIANT name{};
            PropVariantInit(&name);
            if (SUCCEEDED(collection->Item(i, &device)) && device &&
                SUCCEEDED(device->GetId(&id)) && id &&
                SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) && properties &&
                SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) &&
                name.vt == VT_LPWSTR && name.pwszVal) {
                gAudioEndpoints.push_back({id, name.pwszVal});
                SendMessageW(gAudioOutput, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(gAudioEndpoints.back().label.c_str()));
            }
            PropVariantClear(&name);
            if (id) CoTaskMemFree(id);
            if (properties) properties->Release();
            if (device) device->Release();
        }
    }
    if (collection) collection->Release();
    if (enumerator) enumerator->Release();
    if (uninitialize) CoUninitialize();

    const std::wstring saved = savedAudioDeviceId();
    int selected = 0;
    for (size_t i = 1; i < gAudioEndpoints.size(); ++i) {
        if (gAudioEndpoints[i].id == saved) { selected = static_cast<int>(i); break; }
    }
    SendMessageW(gAudioOutput, CB_SETCURSEL, selected, 0);
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        gConfiguredAudioDeviceId = gAudioEndpoints[static_cast<size_t>(selected)].id;
    }
}

void selectAudioEndpoint() {
    const int selected = static_cast<int>(SendMessageW(gAudioOutput, CB_GETCURSEL, 0, 0));
    if (selected < 0 || selected >= static_cast<int>(gAudioEndpoints.size())) return;
    const std::wstring id = gAudioEndpoints[static_cast<size_t>(selected)].id;
    {
        std::lock_guard<std::mutex> lock(gAudioMutex);
        gConfiguredAudioDeviceId = id;
    }
    saveAudioDeviceId(id);
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
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

bool validIpv4(const std::wstring& text) {
    IN_ADDR address{};
    return !text.empty() && InetPtonW(AF_INET, text.c_str(), &address) == 1;
}

bool protectedMode() {
    HWND protectedButton = gMain ? GetDlgItem(gMain, kIdProtected) : nullptr;
    return protectedButton && SendMessageW(protectedButton, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

bool readyToStart() {
    HWND start = gMain ? GetDlgItem(gMain, kIdStart) : nullptr;
    if (!start || !IsWindowEnabled(start)) return false;
    return controlText(start).find(L"Iniciar") != std::wstring::npos;
}

bool transmissionRunning() {
    HWND start = gMain ? GetDlgItem(gMain, kIdStart) : nullptr;
    if (!start) return false;
    return controlText(start).find(L"Parar transmissão") != std::wstring::npos;
}

bool privacyActive() {
    if (trayImageHidden()) return true;
    HWND status = gMain ? GetDlgItem(gMain, kIdStatus) : nullptr;
    if (!status) return false;
    const std::wstring text = controlText(status);
    return text.find(L"rivacidade") != std::wstring::npos || text.find(L"rotegido") != std::wstring::npos;
}

void refreshAudioState() {
    if (!transmissionRunning()) {
        gAudioAllowed = false;
        return;
    }
    if (privacyActive()) {
        gAudioAllowed = false;
        return;
    }
    if (!protectedMode()) {
        gAudioAllowed = gAudioRequested.load();
        return;
    }
    gAudioAllowed = gAudioRequested.load() && gReleased.load();
}

void refreshControls() {
    if (!gIpEdit) return;
    const bool protectedSelected = protectedMode();
    ShowWindow(gIpEdit, protectedSelected ? SW_SHOW : SW_HIDE);
    ShowWindow(gIpLabel, protectedSelected ? SW_SHOW : SW_HIDE);
    ShowWindow(gIpHint, protectedSelected ? SW_SHOW : SW_HIDE);
    EnableWindow(gIpEdit, protectedSelected && readyToStart());
    if (gAudioOption) EnableWindow(gAudioOption, readyToStart());
    if (gAudioOutput) EnableWindow(gAudioOutput, readyToStart());
    refreshAudioState();
}

void rememberConfiguredIp() {
    std::lock_guard<std::mutex> lock(gIpMutex);
    gConfiguredIp = protectedMode() ? utf8(controlText(gIpEdit)) : std::string{};
}

LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                              UINT_PTR, DWORD_PTR) {
    if (msg == WM_COMMAND) {
        const int id = LOWORD(wp);
        if (id == kIdStart) {
            if (readyToStart()) {
                if (protectedMode()) {
                    const std::wstring ip = controlText(gIpEdit);
                    if (!validIpv4(ip)) {
                        MessageBoxW(hwnd,
                            L"Informe um IPv4 válido para a máquina autorizada.\n\nExemplo: 192.168.0.100",
                            L"IP autorizado", MB_OK | MB_ICONWARNING);
                        SetFocus(gIpEdit);
                        return 0;
                    }
                }
                rememberConfiguredIp();
                gAudioRequested = gAudioOption &&
                    SendMessageW(gAudioOption, BM_GETCHECK, 0, 0) == BST_CHECKED;
                gReleased = !protectedMode();
                gAudioAllowed = gAudioRequested.load() && !protectedMode();
            } else {
                gReleased = false;
                gAudioAllowed = false;
            }
        } else if (id == kIdRelease && protectedMode()) {
            gReleased = true;
            refreshAudioState();
        } else if (id == kAudioOptionId && readyToStart()) {
            gAudioRequested = SendMessageW(gAudioOption, BM_GETCHECK, 0, 0) == BST_CHECKED;
            refreshAudioState();
        } else if (id == kAudioOutputId && HIWORD(wp) == CBN_SELCHANGE && readyToStart()) {
            selectAudioEndpoint();
        }
    } else if (msg == WM_TIMER || msg == WM_ENABLE || msg == WM_SHOWWINDOW) {
        refreshControls();
    } else if (msg == WM_NCDESTROY) {
        gReleased = false;
        gAudioAllowed = false;
        RemoveWindowSubclass(hwnd, subclassProc, 1);
        gMain = nullptr;
        gIpEdit = nullptr;
        gIpLabel = nullptr;
        gIpHint = nullptr;
        gAudioOption = nullptr;
        gAudioOutput = nullptr;
        gAudioOutputLabel = nullptr;
        gAudioEndpoints.clear();
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void installUi(HWND hwnd) {
    if (gMain || !hwnd) return;
    gMain = hwnd;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HWND cover = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        536, 184, 374, 68, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(cover, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    gIpLabel = CreateWindowExW(0, L"STATIC", L"IP autorizado", WS_CHILD | WS_VISIBLE,
        540, 190, 102, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIpLabelId)),
        GetModuleHandleW(nullptr), nullptr);
    gIpEdit = CreateWindowExW(0, L"EDIT", L"192.168.0.100",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
        648, 184, 258, 36, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIpEditId)),
        GetModuleHandleW(nullptr), nullptr);
    gIpHint = CreateWindowExW(0, L"STATIC", L"Somente esta máquina poderá receber no Modo protegido.",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        540, 224, 366, 22, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIpHintId)),
        GetModuleHandleW(nullptr), nullptr);

    gAudioOption = CreateWindowExW(0, L"BUTTON", L"Enviar áudio",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        306, 342, 116, 28, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioOptionId)),
        GetModuleHandleW(nullptr), nullptr);
    gAudioOutputLabel = CreateWindowExW(0, L"STATIC", L"Saída de áudio", WS_CHILD | WS_VISIBLE | SS_LEFT,
        32, 578, 120, 18, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioOutputLabelId)),
        GetModuleHandleW(nullptr), nullptr);
    gAudioOutput = CreateWindowExW(0, WC_COMBOBOXW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        32, 598, 466, 120, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioOutputId)),
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(gIpLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gIpEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gIpHint, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioOption, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioOutputLabel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(gAudioOutput, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetWindowTheme(gIpEdit, L"DarkMode_Explorer", nullptr);
    SetWindowTheme(gAudioOption, L"DarkMode_Explorer", nullptr);
    SendMessageW(gAudioOption, BM_SETCHECK, BST_CHECKED, 0);
    refreshAudioEndpoints();
    gAudioRequested = true;
    SetWindowSubclass(hwnd, subclassProc, 1, 0);
    refreshControls();
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK callWndHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && !gMain) {
        const auto* data = reinterpret_cast<CWPSTRUCT*>(lp);
        if (data && data->hwnd && data->message == WM_PAINT) {
            wchar_t className[128]{};
            GetClassNameW(data->hwnd, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, L"TransmissorNDIPortatilV3") == 0) installUi(data->hwnd);
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

struct UiBootstrap {
    UiBootstrap() {
        gHook = SetWindowsHookExW(WH_CALLWNDPROC, callWndHook, nullptr, GetCurrentThreadId());
    }
    ~UiBootstrap() {
        if (gHook) UnhookWindowsHookEx(gHook);
    }
} gBootstrap;
}

std::string configuredReceiverIp() {
    std::lock_guard<std::mutex> lock(gIpMutex);
    return gConfiguredIp;
}

std::wstring configuredAudioDeviceId() {
    std::lock_guard<std::mutex> lock(gAudioMutex);
    return gConfiguredAudioDeviceId;
}

bool audioTransmissionAllowed() {
    return gAudioAllowed.load() && !trayImageHidden();
}
