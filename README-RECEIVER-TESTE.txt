ZOSMA RECEIVER — PROTÓTIPO PORTÁTIL PARA WINDOWS
ZOSMA LABS — https://zosma.com.br

Este primeiro protótipo serve para validar a descoberta e a recepção do vídeo
enviado pelo Zosma Transmitter na rede local.

COMO TESTAR

1. Extraia todo o conteúdo do ZIP para uma pasta.
2. Mantenha "Zosma Receiver.exe" e "Processing.NDI.Lib.x64.dll" juntos.
3. No primeiro computador, abra o Zosma Transmitter e inicie a transmissão.
4. No segundo computador, abra "Zosma Receiver.exe".
5. Aguarde a fonte aparecer na lista. Se necessário, clique em "Atualizar".
6. Selecione a fonte e clique em "Conectar".
7. Se o Transmitter estiver no modo protegido, clique em "Liberar transmissão"
   depois que o Receiver estiver conectado.

Os dois computadores devem estar na mesma rede local. Para o primeiro teste,
prefira conexão por cabo de rede e permita o acesso em redes privadas caso o
Firewall do Windows pergunte.

ESCOPO DESTE PRIMEIRO TESTE

- Descoberta automática de fontes NDI.
- Recepção e prévia de vídeo com renderização em buffer duplo.
- Reprodução do áudio recebido na saída padrão do Windows.
- Exibição da resolução e do FPS recebido.
- Tela cheia, com saída pela tecla Esc.
- Ainda não cria câmera virtual nem fonte direta no OBS.

Este pacote é portátil e não exige a instalação do NDI Tools.
