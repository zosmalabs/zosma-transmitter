#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// A implementação original é compilada como sendFrameRaw(). O método público
// sendFrame() fica em ndi_sender_tray.cpp para poder substituir apenas o vídeo
// quando o usuário escolher "Ocultar imagem" na bandeja.
#ifdef NDI_SENDER_IMPL
#define sendFrame sendFrameRaw
#endif

class NdiSender {
public:
    NdiSender() = default;
    ~NdiSender();
    NdiSender(const NdiSender&) = delete;
    NdiSender& operator=(const NdiSender&) = delete;

    bool open(const std::filesystem::path& runtimeDll, const std::string& sourceName, std::wstring& error);
    void close();
    bool sendFrame(const std::uint8_t* data, int width, int height, int fps);
#ifndef NDI_SENDER_IMPL
    bool sendFrameRaw(const std::uint8_t* data, int width, int height, int fps);
#endif
    int connections() const;
    bool valid() const;

private:
    void* module_{};
    const void* api_{};
    void* sender_{};
    std::vector<std::uint8_t> asyncVideoBuffers_[2];
    int asyncVideoIndex_{0};
    bool asyncVideoSubmitted_{false};
};
