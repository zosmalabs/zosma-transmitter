# Zosma Transmitter

**Transmissão NDI portátil com foco em simplicidade, controle e privacidade.**

Aplicativo da [Zosma Labs](https://zosma.com.br) para transmitir vídeo e áudio pela rede local via NDI® no Windows, sem exigir a instalação do pacote completo do NDI Tools na máquina de origem.

> **Versão atual: 0.3.1 Beta**

[Baixar a versão mais recente](https://github.com/zosmalabs/transmissor-ndi-portatil/releases/latest) · [Site da Zosma](https://zosma.com.br)

## Principais recursos

- Windows 10 ou 11, 64 bits;
- captura de monitor completo ou janela específica;
- transmissão de vídeo e áudio via NDI High Bandwidth;
- opções de 30 e 60 FPS;
- seleção da saída de áudio do Windows;
- nome configurável para a fonte NDI;
- cursor do mouse opcional;
- modos **Rápido** e **Protegido**;
- autorização do receptor antes da liberação da imagem no modo protegido;
- restrição do receptor por endereço IPv4 autorizado;
- proteção automática para WhatsApp, WhatsApp Business, WhatsApp Web, Telegram Desktop e Telegram Web;
- permissões temporárias de privacidade por execução;
- ocultação da imagem sem encerrar a fonte NDI;
- operação em segundo plano pela bandeja do Windows;
- monitoramento da interface de rede e da qualidade da transmissão;
- configurações persistentes entre execuções.

## Modos de transmissão

### Protegido

A fonte NDI pode ser iniciada sem liberar imediatamente a imagem capturada. O aplicativo aguarda o receptor autorizado e a transmissão da imagem é liberada manualmente no computador de origem. Também é possível restringir o receptor por endereço IPv4.

### Rápido

A captura é transmitida imediatamente, indicada para situações em que a rede e os receptores já são conhecidos e controlados.

## Privacidade

O Zosma Transmitter possui proteção para WhatsApp e Telegram. Quando conteúdo protegido é detectado na área efetivamente transmitida, a imagem pode ser temporariamente substituída por uma tela de privacidade sem encerrar a fonte NDI.

As permissões para transmitir esses aplicativos são temporárias e não permanecem autorizadas após reiniciar o programa.

As notificações do Windows não são ocultadas; para apresentações e transmissões, recomenda-se também utilizar o recurso **Não incomodar** do sistema.

## Sobre a versão Beta

Esta é uma versão Beta. Apesar dos testes realizados, ainda podem existir incompatibilidades com determinadas configurações de hardware, drivers, interfaces de áudio, redes ou softwares receptores. Feedback e relatos de problemas são bem-vindos.

Atualmente o Zosma Transmitter está disponível para **Windows**. Versões para macOS e Linux não fazem parte desta release.

## Compilação automática

O workflow **Compilar para Windows** gera os artefatos do aplicativo pelo GitHub Actions. A compilação utiliza as interfaces públicas do NDI SDK e obtém o redistribuível oficial durante a execução do workflow.

O uso das interfaces e do runtime do NDI está sujeito à licença do NDI SDK. O projeto não inclui o instalador completo do SDK nem o pacote NDI Tools.

## Desenvolvimento local

Requisitos:

- Visual Studio 2022 com o workload **Desktop development with C++**;
- CMake 3.24 ou superior;
- `Processing.NDI.Lib.x64.dll` obtida do redistribuível oficial do NDI e colocada ao lado do executável.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Zosma Labs

**Ideias transformadas em software.**

[zosma.com.br](https://zosma.com.br)

## NDI®

O Zosma Transmitter utiliza tecnologia **NDI®** para transmissão de vídeo e áudio em rede.

**Zosma Transmitter é um software independente e não é afiliado, patrocinado, certificado ou desenvolvido pela Vizrt NDI AB.**

NDI® é uma marca registrada da Vizrt NDI AB. Os avisos aplicáveis aos componentes de terceiros acompanham o pacote do aplicativo.

## Licença

Nenhuma licença foi definida para o código do projeto nesta fase Beta. Consulte também `THIRD_PARTY_NOTICES.md` para os componentes de terceiros.
