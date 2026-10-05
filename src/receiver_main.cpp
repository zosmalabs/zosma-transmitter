#include <windows.h>
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
    const int controlsHeight = 76;
    const int statusHeight = 54;
    const int contentWidth = std::max(100, width - margin * 2);

    const int sourceWidth = std::max(180, contentWidth - 650);
    MoveWindow(g.sources, margin, headerHeight, sourceWidth, 38, TRUE);
    MoveWindow(g.monitors, margin + sourceWidth + 10, headerHeight, 190, 200, TRUE);
    MoveWindow(g.audio, margin + sourceWidth + 210, headerHeight + 8, 120, 24, TRUE);
    MoveWindow(g.refresh, width - margin - 310, headerHeight, 90, 38, TRUE);
    MoveWindow(g.connect, width - margin - 210, headerHeight, 100, 38, TRUE);
    MoveWindow(g.fullscreen, width - margin - 100, headerHeight, 100, 38, TRUE);

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
        SetStretchBltMode(backBuffer, COLORONCOLOR);
        StretchDIBits(backBuffer, x, y, drawWidth, drawHeight, 0, 0, sourceWidth, sourceHeight,
                      frame->pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
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
        g.sources = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdSources), nullptr, nullptr);
        g.refresh = CreateWindowExW(0, WC_BUTTONW, L"Atualizar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdRefresh), nullptr, nullptr);
        g.connect = CreateWindowExW(0, WC_BUTTONW, L"Conectar", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdConnect), nullptr, nullptr);
        g.fullscreen = CreateWindowExW(0, WC_BUTTONW, L"Tela cheia", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                       0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdFullscreen), nullptr, nullptr);
        g.monitors = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdMonitors), nullptr, nullptr);
        g.audio = CreateWindowExW(0, WC_BUTTONW, L"Reproduzir áudio", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                  0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdAudio), nullptr, nullptr);
        g.preview = CreateWindowExW(0, kPreviewClass, nullptr, WS_CHILD | WS_VISIBLE,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdPreview), nullptr, nullptr);
        g.status = CreateWindowExW(0, WC_STATICW, L"Inicializando…", WS_CHILD | WS_VISIBLE,
                                   0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdStatus), nullptr, nullptr);
        g.details = CreateWindowExW(0, WC_STATICW, L"Teste inicial de recepção de vídeo", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                    0, 0, 0, 0, window, reinterpret_cast<HMENU>(IdDetails), nullptr, nullptr);

        for (HWND control : {g.sources, g.monitors, g.audio, g.refresh, g.connect, g.fullscreen, g.status, g.details}) {
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
            g.audioEnabled = SendMessageW(g.audio, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
