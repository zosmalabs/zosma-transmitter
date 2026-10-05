#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
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
constexpr wchar_t kOutputClass[] = L"ZosmaReceiverOutputV1";
constexpr UINT kFrameReady = WM_APP + 1;
constexpr UINT kReceiverStopped = WM_APP + 2;
constexpr UINT_PTR kDiscoveryTimer = 100;
constexpr UINT_PTR kReconnectTimer = 101;

constexpr COLORREF kBackground = RGB(10, 15, 24);
constexpr COLORREF kPanel = RGB(19, 27, 39);
constexpr COLORREF kBorder = RGB(45, 58, 76);
constexpr COLORREF kControl = RGB(25, 36, 51);
constexpr COLORREF kControlPressed = RGB(34, 48, 66);
constexpr COLORREF kAccentBright = RGB(45, 211, 143);
constexpr COLORREF kText = RGB(241, 245, 249);
constexpr COLORREF kMuted = RGB(148, 163, 184);
constexpr COLORREF kAccent = RGB(38, 177, 120);

enum ControlId {
    IdSources = 1001,
    IdRefresh,
    IdConnect,
    IdFullscreen,
    IdMonitors,
    IdAudio,
    IdResetCrop,
    IdPreview,
    IdStatus,
    IdDetails
};

struct SourceInfo {
    std::string name;
    std::string url;
};

struct MonitorInfo {
    std::wstring name;
    RECT bounds{};
    bool primary{};
};

struct VideoFrameBuffer {
    std::vector<std::uint8_t> pixels;
    int width{};
    int height{};
};

class PreviewBackBuffer {
public:
    ~PreviewBackBuffer() { reset(); }

    bool ensure(HDC reference, int width, int height) {
        if (dc_ && bitmap_ && width_ == width && height_ == height) return true;
        reset();
        dc_ = CreateCompatibleDC(reference);
        if (!dc_) return false;
        bitmap_ = CreateCompatibleBitmap(reference, width, height);
        if (!bitmap_) {
            reset();
            return false;
        }
        previousBitmap_ = SelectObject(dc_, bitmap_);
        width_ = width;
        height_ = height;
        return true;
    }

    HDC dc() const { return dc_; }

    void reset() {
        if (dc_ && previousBitmap_) SelectObject(dc_, previousBitmap_);
        if (bitmap_) DeleteObject(bitmap_);
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr;
        bitmap_ = nullptr;
        previousBitmap_ = nullptr;
        width_ = 0;
        height_ = 0;
    }

private:
    HDC dc_{};
    HBITMAP bitmap_{};
    HGDIOBJ previousBitmap_{};
    int width_{};
    int height_{};
};

class WaveOutPlayer {
public:
    ~WaveOutPlayer() { close(); }

    void stop() { close(); }

    bool submit(const NDIlib_audio_frame_v3_t& audio) {
        if (!audio.p_data || audio.sample_rate <= 0 || audio.no_channels <= 0 || audio.no_samples <= 0)
            return false;
        if (!device_ || sampleRate_ != audio.sample_rate || channels_ != audio.no_channels) {
            close();
            if (!open(audio.sample_rate, audio.no_channels)) return false;
        }

        releaseCompleted();
        if (started_ && blocks_.empty()) started_ = false;
        // Mantém a latência limitada mesmo quando o dispositivo fica ocupado.
        if (blocks_.size() + pending_.size() >= 24) return false;

        auto block = std::make_unique<Block>();
        const size_t sampleCount = static_cast<size_t>(audio.no_samples) * static_cast<size_t>(audio.no_channels);
        block->samples.resize(sampleCount);
        const int stride = audio.channel_stride_in_bytes > 0
            ? audio.channel_stride_in_bytes
            : audio.no_samples * static_cast<int>(sizeof(float));

        for (int sample = 0; sample < audio.no_samples; ++sample) {
            for (int channel = 0; channel < audio.no_channels; ++channel) {
                const auto* channelData = reinterpret_cast<const float*>(audio.p_data +
                    static_cast<size_t>(channel) * static_cast<size_t>(stride));
                const float value = std::clamp(channelData[sample], -1.0f, 1.0f);
                block->samples[static_cast<size_t>(sample) * audio.no_channels + channel] =
                    static_cast<std::int16_t>(std::lround(value * 32767.0f));
            }
        }

        pendingSamples_ += audio.no_samples;
        pending_.push_back(std::move(block));

        // Um pequeno buffer inicial absorve a variação de entrega da rede sem
        // transformar a saída em uma reprodução perceptivelmente atrasada.
        const int bufferedMilliseconds = static_cast<int>(pendingSamples_ * 1000LL / sampleRate_);
        if (!started_ && bufferedMilliseconds < 80) return true;

        while (!pending_.empty()) {
            auto next = std::move(pending_.front());
            pending_.erase(pending_.begin());
            if (!queue(std::move(next))) {
                pending_.clear();
                pendingSamples_ = 0;
                return false;
            }
        }
        pendingSamples_ = 0;
        started_ = true;
        return true;
    }

private:
    struct Block {
        WAVEHDR header{};
        std::vector<std::int16_t> samples;
    };

    bool open(int sampleRate, int channels) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = static_cast<WORD>(channels);
        format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
        format.wBitsPerSample = 16;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        if (waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
            device_ = nullptr;
            return false;
        }
        sampleRate_ = sampleRate;
        channels_ = channels;
        return true;
    }

    bool queue(std::unique_ptr<Block> block) {
        block->header.lpData = reinterpret_cast<LPSTR>(block->samples.data());
        block->header.dwBufferLength = static_cast<DWORD>(block->samples.size() * sizeof(std::int16_t));
        if (waveOutPrepareHeader(device_, &block->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
            return false;
        if (waveOutWrite(device_, &block->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(device_, &block->header, sizeof(WAVEHDR));
            return false;
        }
        blocks_.push_back(std::move(block));
        return true;
    }

    void releaseCompleted() {
        for (auto it = blocks_.begin(); it != blocks_.end();) {
            if (((*it)->header.dwFlags & WHDR_DONE) == 0) {
                ++it;
                continue;
            }
            waveOutUnprepareHeader(device_, &(*it)->header, sizeof(WAVEHDR));
            it = blocks_.erase(it);
        }
    }

    void close() {
        if (!device_) return;
        waveOutReset(device_);
        for (auto& block : blocks_)
            waveOutUnprepareHeader(device_, &block->header, sizeof(WAVEHDR));
        blocks_.clear();
        pending_.clear();
        waveOutClose(device_);
        device_ = nullptr;
        sampleRate_ = 0;
        channels_ = 0;
        pendingSamples_ = 0;
        started_ = false;
    }

    HWAVEOUT device_{};
    int sampleRate_{};
    int channels_{};
    std::vector<std::unique_ptr<Block>> blocks_;
    std::vector<std::unique_ptr<Block>> pending_;
    std::int64_t pendingSamples_{};
    bool started_{};
};

struct State {
    HWND window{};
    HWND sources{};
    HWND refresh{};
    HWND connect{};
    HWND fullscreen{};
    HWND monitors{};
    HWND audio{};
    HWND resetCrop{};
    HWND preview{};
    HWND output{};
    HWND status{};
    HWND details{};

    HMODULE ndiModule{};
    const NDIlib_v6* ndi{};
    NDIlib_find_instance_t finder{};
    NDIlib_recv_instance_t receiver{};

    std::vector<SourceInfo> sourceList;
    std::vector<MonitorInfo> monitorList;
    std::atomic_bool connected{false};
    std::atomic_bool stopRequested{false};
    std::thread videoWorker;
    std::thread audioWorker;

    std::mutex frameMutex;
    std::array<std::shared_ptr<VideoFrameBuffer>, 3> framePool;
    std::shared_ptr<VideoFrameBuffer> latestFrame;
    size_t nextFrameBuffer{};
    int frameWidth{};
    int frameHeight{};
    std::atomic_int fps{0};
    std::uint64_t framesReceived{};
    std::atomic_bool audioActive{false};
    std::atomic_bool audioEnabled{true};
    std::atomic_bool frameMessagePending{false};

    int cropLeft{};
    int cropTop{};
    int cropRight{};
    int cropBottom{};
    int cropSourceWidth{};
    int cropSourceHeight{};
    RECT previewImageRect{};
    int cropDragEdge{};

};

State g;
HBRUSH gBackgroundBrush{};
HBRUSH gPanelBrush{};
HFONT gFont{};
HFONT gFontSmall{};
HFONT gFontBold{};
HFONT gFontTitle{};
PreviewBackBuffer gPreviewBackBuffer;
PreviewBackBuffer gOutputBackBuffer;

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

void roundControl(HWND control, int width, int height, int radius = 16) {
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    SetWindowRgn(control, region, TRUE);
}

void drawRoundedButton(const DRAWITEMSTRUCT& item) {
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool primary = item.CtlID == IdFullscreen;
    const bool audio = item.CtlID == IdAudio;

    RECT bounds = item.rcItem;
    HBRUSH background = CreateSolidBrush(audio ? kBackground :
                                         (primary ? (pressed ? RGB(28, 150, 99) : kAccent)
                                                  : (pressed ? kControlPressed : kControl)));
    HPEN border = CreatePen(PS_SOLID, 1, audio ? kBackground : (primary ? kAccentBright : kBorder));
    HGDIOBJ oldBrush = SelectObject(item.hDC, background);
    HGDIOBJ oldPen = SelectObject(item.hDC, border);
    RoundRect(item.hDC, bounds.left, bounds.top, bounds.right, bounds.bottom, 14, 14);
    SelectObject(item.hDC, oldPen);
    SelectObject(item.hDC, oldBrush);
    DeleteObject(border);
    DeleteObject(background);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(93, 108, 128) : kText);
    SelectObject(item.hDC, gFont);

    if (audio) {
        const bool checked = SendMessageW(item.hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
        RECT box{bounds.left + 2, bounds.top + 6, bounds.left + 38, bounds.top + 26};
        HBRUSH checkBrush = CreateSolidBrush(checked ? kAccent : RGB(36, 49, 66));
        HPEN checkPen = CreatePen(PS_SOLID, 1, checked ? kAccentBright : kBorder);
        oldBrush = SelectObject(item.hDC, checkBrush);
        oldPen = SelectObject(item.hDC, checkPen);
        RoundRect(item.hDC, box.left, box.top, box.right, box.bottom, 20, 20);
        HBRUSH knob = CreateSolidBrush(RGB(255, 255, 255));
        SelectObject(item.hDC, knob);
        SelectObject(item.hDC, GetStockObject(NULL_PEN));
        const int knobLeft = checked ? box.right - 18 : box.left + 3;
        Ellipse(item.hDC, knobLeft, box.top + 3, knobLeft + 14, box.top + 17);
        SelectObject(item.hDC, oldPen);
        SelectObject(item.hDC, oldBrush);
        DeleteObject(knob);
        DeleteObject(checkPen);
        DeleteObject(checkBrush);
        RECT label{bounds.left + 48, bounds.top, bounds.right - 4, bounds.bottom};
        DrawTextW(item.hDC, L"Reproduzir áudio", -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    } else {
        wchar_t label[128]{};
        GetWindowTextW(item.hwndItem, label, 128);
        DrawTextW(item.hDC, label, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (item.itemState & ODS_FOCUS) {
        RECT focus = bounds;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(item.hDC, &focus);
    }
}

void drawDarkComboItem(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH background = CreateSolidBrush(selected ? RGB(31, 107, 82) : kControl);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    wchar_t label[512]{};
    SendMessageW(item.hwndItem, CB_GETLBTEXT, item.itemID, reinterpret_cast<LPARAM>(label));
    RECT textRect = item.rcItem;
    textRect.left += 12;
    textRect.right -= 8;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, kText);
    SelectObject(item.hDC, gFont);
    DrawTextW(item.hDC, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

std::shared_ptr<VideoFrameBuffer> acquireFrameBuffer() {
    std::lock_guard lock(g.frameMutex);
    for (size_t offset = 0; offset < g.framePool.size(); ++offset) {
        const size_t index = (g.nextFrameBuffer + offset) % g.framePool.size();
        auto& candidate = g.framePool[index];
        if (!candidate) candidate = std::make_shared<VideoFrameBuffer>();
        if (candidate.use_count() == 1) {
            g.nextFrameBuffer = (index + 1) % g.framePool.size();
            return candidate;
        }
    }

    const size_t index = g.nextFrameBuffer;
    auto replacement = std::make_shared<VideoFrameBuffer>();
    g.framePool[index] = replacement;
    g.nextFrameBuffer = (index + 1) % g.framePool.size();
    return replacement;
}

void videoReceiveLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    auto started = std::chrono::steady_clock::now();
    int intervalFrames = 0;
    while (!g.stopRequested.load()) {
        NDIlib_video_frame_v2_t video{};
        const NDIlib_frame_type_e type = g.ndi->recv_capture_v3(g.receiver, &video, nullptr, nullptr, 100);
        if (type == NDIlib_frame_type_video) {
            if (video.p_data && video.xres > 0 && video.yres > 0) {
                const size_t rowBytes = static_cast<size_t>(video.xres) * 4u;
                auto frame = acquireFrameBuffer();
                frame->pixels.resize(rowBytes * static_cast<size_t>(video.yres));
                frame->width = video.xres;
                frame->height = video.yres;
                const auto* source = video.p_data;
                const int stride = video.line_stride_in_bytes != 0 ? video.line_stride_in_bytes : video.xres * 4;
                for (int row = 0; row < video.yres; ++row) {
                    const int sourceRow = stride >= 0 ? row : (video.yres - 1 - row);
                    std::copy_n(source + static_cast<std::ptrdiff_t>(sourceRow) * std::abs(stride), rowBytes,
                                frame->pixels.data() + static_cast<size_t>(row) * rowBytes);
                }
                {
                    std::lock_guard lock(g.frameMutex);
                    g.latestFrame = std::move(frame);
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
                if (!g.frameMessagePending.exchange(true))
                    PostMessageW(g.window, kFrameReady, 0, 0);
            }
            g.ndi->recv_free_video_v2(g.receiver, &video);
        } else if (type == NDIlib_frame_type_error) {
            break;
        }
    }
    PostMessageW(g.window, kReceiverStopped, 0, 0);
}

void audioReceiveLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    WaveOutPlayer audioPlayer;
    while (!g.stopRequested.load()) {
        NDIlib_audio_frame_v3_t audio{};
        const NDIlib_frame_type_e type = g.ndi->recv_capture_v3(g.receiver, nullptr, &audio, nullptr, 100);
        if (type == NDIlib_frame_type_audio) {
            if (g.audioEnabled.load()) {
                if (audioPlayer.submit(audio)) g.audioActive = true;
            } else {
                audioPlayer.stop();
                g.audioActive = false;
            }
            g.ndi->recv_free_audio_v3(g.receiver, &audio);
        } else if (type == NDIlib_frame_type_error) {
            break;
        }
    }
}

void disconnectReceiver(bool updateUi = true) {
    g.stopRequested = true;
    if (g.videoWorker.joinable()) g.videoWorker.join();
    if (g.audioWorker.joinable()) g.audioWorker.join();
    if (g.ndi && g.receiver) g.ndi->recv_destroy(g.receiver);
    g.receiver = nullptr;
    g.connected = false;
    g.stopRequested = false;
    g.audioActive = false;
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
        g.latestFrame.reset();
        for (auto& frame : g.framePool) frame.reset();
        g.nextFrameBuffer = 0;
        g.frameWidth = 0;
        g.frameHeight = 0;
        g.framesReceived = 0;
        g.fps = 0;
        g.audioActive = false;
        g.frameMessagePending = false;
    }
    g.connected = true;
    g.stopRequested = false;
    EnableWindow(g.sources, FALSE);
    EnableWindow(g.refresh, FALSE);
    SetWindowTextW(g.connect, L"Desconectar");
    setStatus(L"Conectando…", wide(selected.name));
    g.videoWorker = std::thread(videoReceiveLoop);
    g.audioWorker = std::thread(audioReceiveLoop);
}

BOOL CALLBACK enumerateMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    MonitorInfo item;
    item.bounds = info.rcMonitor;
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int height = info.rcMonitor.bottom - info.rcMonitor.top;
    item.name = L"Monitor " + std::to_wstring(g.monitorList.size() + 1) + L" — " +
                std::to_wstring(width) + L" × " + std::to_wstring(height) +
                (item.primary ? L" (principal)" : L"");
    g.monitorList.push_back(std::move(item));
    return TRUE;
}

void refreshMonitors() {
    const int previous = static_cast<int>(SendMessageW(g.monitors, CB_GETCURSEL, 0, 0));
    g.monitorList.clear();
    SendMessageW(g.monitors, CB_RESETCONTENT, 0, 0);
    EnumDisplayMonitors(nullptr, nullptr, enumerateMonitor, 0);
    for (const auto& monitor : g.monitorList)
        SendMessageW(g.monitors, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(monitor.name.c_str()));

    int selection = previous;
    if (selection < 0 || selection >= static_cast<int>(g.monitorList.size())) {
        selection = 0;
        for (size_t index = 0; index < g.monitorList.size(); ++index) {
            if (!g.monitorList[index].primary) {
                selection = static_cast<int>(index);
                break;
            }
        }
    }
    if (!g.monitorList.empty()) SendMessageW(g.monitors, CB_SETCURSEL, selection, 0);
}

void closeOutput() {
    if (g.output) DestroyWindow(g.output);
}

void toggleOutput() {
    if (g.output) {
        closeOutput();
        return;
    }
    refreshMonitors();
    const int selection = static_cast<int>(SendMessageW(g.monitors, CB_GETCURSEL, 0, 0));
    if (selection < 0 || selection >= static_cast<int>(g.monitorList.size())) return;
    const RECT bounds = g.monitorList[selection].bounds;
    g.output = CreateWindowExW(WS_EX_TOPMOST, kOutputClass, L"Zosma Receiver — Telão",
                               WS_POPUP | WS_VISIBLE, bounds.left, bounds.top,
                               bounds.right - bounds.left, bounds.bottom - bounds.top,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g.output) {
        SetWindowTextW(g.fullscreen, L"Fechar telão");
        SetForegroundWindow(g.output);
    }
}

void layoutControls(int width, int height) {
    const int margin = 24;
    const int headerHeight = 76;
    const int controlsHeight = 104;
    const int statusHeight = 54;
    const int contentWidth = std::max(100, width - margin * 2);

    // Ações e fonte ficam na primeira linha. Monitor e áudio usam uma segunda
    // linha para que os textos não sejam truncados em telas menores.
    const int sourceWidth = std::max(260, contentWidth - 336);
    MoveWindow(g.sources, margin, headerHeight, sourceWidth, 38, TRUE);
    MoveWindow(g.refresh, width - margin - 326, headerHeight, 96, 38, TRUE);
    MoveWindow(g.connect, width - margin - 220, headerHeight, 110, 38, TRUE);
    MoveWindow(g.fullscreen, width - margin - 100, headerHeight, 100, 38, TRUE);

    const int secondRow = headerHeight + 46;
    MoveWindow(g.monitors, margin, secondRow, std::min(360, contentWidth - 210), 240, TRUE);
    const int monitorWidth = std::min(360, contentWidth - 210);
    MoveWindow(g.audio, margin + monitorWidth + 16, secondRow + 3, 180, 32, TRUE);
    MoveWindow(g.resetCrop, margin + monitorWidth + 206, secondRow, 130, 38, TRUE);

    roundControl(g.sources, sourceWidth, 38);
    roundControl(g.monitors, monitorWidth, 38);
    roundControl(g.audio, 180, 32);
    roundControl(g.resetCrop, 130, 38);
    roundControl(g.refresh, 96, 38);
    roundControl(g.connect, 110, 38);
    roundControl(g.fullscreen, 100, 38);

    const int previewTop = headerHeight + controlsHeight;
    const int previewHeight = std::max(120, height - previewTop - statusHeight - margin);
    MoveWindow(g.preview, margin, previewTop, contentWidth, previewHeight, TRUE);
    MoveWindow(g.status, margin, height - statusHeight, contentWidth / 2, 24, TRUE);
    MoveWindow(g.details, margin + contentWidth / 2, height - statusHeight, contentWidth / 2, 24, TRUE);
}

void ensureCropBounds(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (g.cropSourceWidth != width || g.cropSourceHeight != height ||
        g.cropRight <= g.cropLeft || g.cropBottom <= g.cropTop) {
        g.cropLeft = 0;
        g.cropTop = 0;
        g.cropRight = width;
        g.cropBottom = height;
        g.cropSourceWidth = width;
        g.cropSourceHeight = height;
    }
}

RECT cropScreenRect() {
    const RECT image = g.previewImageRect;
    const int width = std::max(1, static_cast<int>(image.right - image.left));
    const int height = std::max(1, static_cast<int>(image.bottom - image.top));
    RECT result{};
    result.left = image.left + MulDiv(g.cropLeft, width, std::max(1, g.cropSourceWidth));
    result.top = image.top + MulDiv(g.cropTop, height, std::max(1, g.cropSourceHeight));
    result.right = image.left + MulDiv(g.cropRight, width, std::max(1, g.cropSourceWidth));
    result.bottom = image.top + MulDiv(g.cropBottom, height, std::max(1, g.cropSourceHeight));
    return result;
}

void drawCropOverlay(HDC dc) {
    const RECT crop = cropScreenRect();
    HPEN pen = CreatePen(PS_SOLID, 2, kAccentBright);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    Rectangle(dc, crop.left, crop.top, crop.right, crop.bottom);

    HBRUSH handle = CreateSolidBrush(kAccentBright);
    SelectObject(dc, handle);
    SelectObject(dc, GetStockObject(NULL_PEN));
    const int centerX = (crop.left + crop.right) / 2;
    const int centerY = (crop.top + crop.bottom) / 2;
    Rectangle(dc, crop.left - 4, centerY - 12, crop.left + 5, centerY + 12);
    Rectangle(dc, crop.right - 5, centerY - 12, crop.right + 4, centerY + 12);
    Rectangle(dc, centerX - 12, crop.top - 4, centerX + 12, crop.top + 5);
    Rectangle(dc, centerX - 12, crop.bottom - 5, centerX + 12, crop.bottom + 4);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(handle);
    DeleteObject(pen);
}

int hitCropEdge(int x, int y) {
    const RECT crop = cropScreenRect();
    constexpr int tolerance = 14;
    if (y >= crop.top - tolerance && y <= crop.bottom + tolerance) {
        if (std::abs(x - crop.left) <= tolerance) return 1;
        if (std::abs(x - crop.right) <= tolerance) return 2;
    }
    if (x >= crop.left - tolerance && x <= crop.right + tolerance) {
        if (std::abs(y - crop.top) <= tolerance) return 3;
        if (std::abs(y - crop.bottom) <= tolerance) return 4;
    }
    return 0;
}

void dragCropEdge(int x, int y) {
    const RECT image = g.previewImageRect;
    const int displayWidth = std::max(1, static_cast<int>(image.right - image.left));
    const int displayHeight = std::max(1, static_cast<int>(image.bottom - image.top));
    const int sourceX = std::clamp(MulDiv(x - image.left, g.cropSourceWidth, displayWidth), 0, g.cropSourceWidth);
    const int sourceY = std::clamp(MulDiv(y - image.top, g.cropSourceHeight, displayHeight), 0, g.cropSourceHeight);
    const int minWidth = std::max(32, g.cropSourceWidth / 20);
    const int minHeight = std::max(32, g.cropSourceHeight / 20);
    if (g.cropDragEdge == 1) g.cropLeft = std::min(sourceX, g.cropRight - minWidth);
    if (g.cropDragEdge == 2) g.cropRight = std::max(sourceX, g.cropLeft + minWidth);
    if (g.cropDragEdge == 3) g.cropTop = std::min(sourceY, g.cropBottom - minHeight);
    if (g.cropDragEdge == 4) g.cropBottom = std::max(sourceY, g.cropTop + minHeight);
    InvalidateRect(g.preview, nullptr, FALSE);
    if (g.output) InvalidateRect(g.output, nullptr, FALSE);
}

void paintPreview(HWND window) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int clientWidth = std::max(1L, client.right - client.left);
    const int clientHeight = std::max(1L, client.bottom - client.top);

    // O bitmap é mantido entre quadros: em 1080p60, recriá-lo a cada pintura
    // custa tempo suficiente para causar perda de fluidez e atrasar o áudio.
    PreviewBackBuffer& buffer = window == g.output ? gOutputBackBuffer : gPreviewBackBuffer;
    if (!buffer.ensure(dc, clientWidth, clientHeight)) {
        FillRect(dc, &client, gPanelBrush);
        EndPaint(window, &paint);
        return;
    }
    HDC backBuffer = buffer.dc();
    FillRect(backBuffer, &client, gPanelBrush);

    std::shared_ptr<VideoFrameBuffer> frame;
    int sourceWidth = 0;
    int sourceHeight = 0;
    {
        std::lock_guard lock(g.frameMutex);
        frame = g.latestFrame;
        if (frame) {
            sourceWidth = frame->width;
            sourceHeight = frame->height;
        }
    }

    if (frame && !frame->pixels.empty() && sourceWidth > 0 && sourceHeight > 0) {
        ensureCropBounds(sourceWidth, sourceHeight);
        const bool outputWindow = window == g.output;
        const int cropLeft = outputWindow ? g.cropLeft : 0;
        const int cropTop = outputWindow ? g.cropTop : 0;
        const int cropWidth = outputWindow ? g.cropRight - g.cropLeft : sourceWidth;
        const int cropHeight = outputWindow ? g.cropBottom - g.cropTop : sourceHeight;
        const int targetWidth = client.right - client.left;
        const int targetHeight = client.bottom - client.top;
        const double scale = std::min(static_cast<double>(targetWidth) / cropWidth,
                                      static_cast<double>(targetHeight) / cropHeight);
        const int drawWidth = std::max(1, static_cast<int>(cropWidth * scale));
        const int drawHeight = std::max(1, static_cast<int>(cropHeight * scale));
        const int x = (targetWidth - drawWidth) / 2;
        const int y = (targetHeight - drawHeight) / 2;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = sourceWidth;
        info.bmiHeader.biHeight = -sourceHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(backBuffer, COLORONCOLOR);
        StretchDIBits(backBuffer, x, y, drawWidth, drawHeight,
                      cropLeft, cropTop, cropWidth, cropHeight,
                      frame->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        if (!outputWindow) {
            g.previewImageRect = RECT{x, y, x + drawWidth, y + drawHeight};
            drawCropOverlay(backBuffer);
        }
    } else {
        SetBkMode(backBuffer, TRANSPARENT);
        SetTextColor(backBuffer, kMuted);
        SelectObject(backBuffer, gFontBold);
        DrawTextW(backBuffer, g.connected.load() ? L"Aguardando o primeiro quadro…" : L"Selecione uma fonte NDI para iniciar",
                  -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (window != g.output)
        FrameRect(backBuffer, &client, GetSysColorBrush(COLOR_WINDOWFRAME));
    BitBlt(dc, 0, 0, clientWidth, clientHeight, backBuffer, 0, 0, SRCCOPY);
    EndPaint(window, &paint);
}

LRESULT CALLBACK previewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        paintPreview(window);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        g.cropDragEdge = hitCropEdge(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (g.cropDragEdge) {
            SetCapture(window);
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE && g.cropDragEdge && GetCapture() == window) {
        dragCropEdge(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    }
    if (message == WM_LBUTTONUP && GetCapture() == window) {
        ReleaseCapture();
        g.cropDragEdge = 0;
        return 0;
    }
    if (message == WM_SETCURSOR) {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(window, &cursor);
        const int edge = hitCropEdge(cursor.x, cursor.y);
        if (edge == 1 || edge == 2) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        if (edge == 3 || edge == 4) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
            return TRUE;
        }
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK outputProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        paintPreview(window);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_DESTROY:
        gOutputBackBuffer.reset();
        if (g.output == window) g.output = nullptr;
        if (g.fullscreen) SetWindowTextW(g.fullscreen, L"Exibir telão");
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        g.window = window;
        g.sources = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdSources), nullptr, nullptr);
        g.refresh = CreateWindowExW(0, WC_BUTTONW, L"Atualizar", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdRefresh), nullptr, nullptr);
        g.connect = CreateWindowExW(0, WC_BUTTONW, L"Conectar", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdConnect), nullptr, nullptr);
        g.fullscreen = CreateWindowExW(0, WC_BUTTONW, L"Tela cheia", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                       0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdFullscreen), nullptr, nullptr);
        g.monitors = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST |
                                     CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                     0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdMonitors), nullptr, nullptr);
        g.audio = CreateWindowExW(0, WC_BUTTONW, L"Reproduzir áudio", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                  0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdAudio), nullptr, nullptr);
        g.resetCrop = CreateWindowExW(0, WC_BUTTONW, L"Remover corte", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                      0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdResetCrop), nullptr, nullptr);
        g.preview = CreateWindowExW(0, kPreviewClass, nullptr, WS_CHILD | WS_VISIBLE,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdPreview), nullptr, nullptr);
        g.status = CreateWindowExW(0, WC_STATICW, L"Inicializando…", WS_CHILD | WS_VISIBLE,
                                   0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdStatus), nullptr, nullptr);
        g.details = CreateWindowExW(0, WC_STATICW, L"Teste inicial de recepção de vídeo", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdDetails), nullptr, nullptr);

        for (HWND control : {g.sources, g.monitors, g.audio, g.resetCrop, g.refresh, g.connect,
                             g.fullscreen, g.status, g.details}) {
            setFont(control, gFont);
            SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
        }
        setFont(g.status, gFontBold);
        SendMessageW(g.audio, BM_SETCHECK, BST_CHECKED, 0);
        SetWindowTextW(g.fullscreen, L"Exibir telão");
        refreshMonitors();
        SetTimer(window, kDiscoveryTimer, 1000, nullptr);
        return 0;
    }
    case WM_SIZE:
        layoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 900;
        limits->ptMinTrackSize.y = 600;
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdRefresh:
            refreshSources();
            return 0;
        case IdConnect:
            connectReceiver();
            return 0;
        case IdFullscreen:
            toggleOutput();
            return 0;
        case IdAudio:
            g.audioEnabled = !g.audioEnabled.load();
            SendMessageW(g.audio, BM_SETCHECK, g.audioEnabled.load() ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(g.audio, nullptr, TRUE);
            return 0;
        case IdResetCrop:
            if (g.cropSourceWidth > 0 && g.cropSourceHeight > 0) {
                g.cropLeft = 0;
                g.cropTop = 0;
                g.cropRight = g.cropSourceWidth;
                g.cropBottom = g.cropSourceHeight;
                InvalidateRect(g.preview, nullptr, FALSE);
                if (g.output) InvalidateRect(g.output, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kDiscoveryTimer && !g.connected.load()) refreshSources();
        if (wParam == kReconnectTimer) {
            KillTimer(window, kReconnectTimer);
            if (!g.connected.load()) connectReceiver();
        }
        return 0;
    case kFrameReady: {
        g.frameMessagePending = false;
        InvalidateRect(g.preview, nullptr, FALSE);
        if (g.output) InvalidateRect(g.output, nullptr, FALSE);
        std::wstring details;
        {
            std::lock_guard lock(g.frameMutex);
            details = std::to_wstring(g.frameWidth) + L" × " + std::to_wstring(g.frameHeight) +
                      L"  •  " + std::to_wstring(g.fps.load()) + L" FPS" +
                      (g.audioActive.load() ? L"  •  Áudio ativo" : L"");
        }
        setStatus(L"Recebendo vídeo", details);
        return 0;
    }
    case kReceiverStopped:
        if (g.connected.load() && !g.stopRequested.load()) {
            setStatus(L"Sinal interrompido", L"Reconectando automaticamente…");
            disconnectReceiver(false);
            SetTimer(window, kReconnectTimer, 1000, nullptr);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kBackground);
        SetTextColor(dc, reinterpret_cast<HWND>(lParam) == g.status ? kText : kMuted);
        return reinterpret_cast<LRESULT>(gBackgroundBrush);
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kControl);
        SetTextColor(dc, kText);
        return reinterpret_cast<LRESULT>(gPanelBrush);
    }
    case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (measure->CtlType == ODT_COMBOBOX) {
            measure->itemHeight = 34;
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (item->CtlType == ODT_BUTTON) {
            drawRoundedButton(*item);
            return TRUE;
        }
        if (item->CtlType == ODT_COMBOBOX) {
            drawDarkComboItem(*item);
            return TRUE;
        }
        break;
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
        KillTimer(window, kReconnectTimer);
        closeOutput();
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

    WNDCLASSEXW outputClass{sizeof(WNDCLASSEXW)};
    outputClass.style = CS_HREDRAW | CS_VREDRAW;
    outputClass.lpfnWndProc = outputProc;
    outputClass.hInstance = instance;
    outputClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    outputClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    outputClass.lpszClassName = kOutputClass;
    RegisterClassExW(&outputClass);

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
