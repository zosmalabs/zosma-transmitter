#pragma once

#include <cstdint>
#include <string>

// Integração da bandeja do Windows e do modo manual "Ocultar imagem".
// O estado não é persistido entre transmissões.
bool trayImageHidden();
void traySetImageHidden(bool hidden);
void traySetSourceName(const std::string& sourceNameUtf8);
void trayResetTransmissionState();

// Retorna um quadro BGRX 1280x720 quando a imagem estiver oculta.
// O ponteiro permanece válido até traySetSourceName()/trayResetTransmissionState().
bool trayHiddenFrame(const std::uint8_t*& data, int& width, int& height);
