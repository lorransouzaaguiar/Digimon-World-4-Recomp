Digimon World 4 - Recomp @VERSION@  (Windows x86-64)  --  EXPERIMENTAL
======================================================================

Port nativo NAO OFICIAL de Digimon World 4 (Xbox, 2005).
Unofficial native port of Digimon World 4 (Xbox, 2005).

@URL@

ESTA E UMA VERSAO DE TESTE. Ate agora o build de Windows so rodou no Linux,
dentro do Wine; em um Windows de verdade ele ainda nao foi testado.
THIS IS A TEST BUILD. So far the Windows build has only run on Linux, inside
Wine; it has not been tested on a real Windows machine yet.


Como jogar / How to play
------------------------

1. Extraia este .zip inteiro para uma pasta em que voce possa gravar
   (nao dentro de "Arquivos de Programas").
   Extract this whole .zip to a folder you can write to (not "Program Files").

2. Copie os arquivos do SEU disco de Digimon World 4 (Xbox, NTSC-U), ja
   extraido, para a pasta game_data:  default.xbe, area.afs, ndmw.afs, ndmw.igb
   Copy the files of YOUR OWN Digimon World 4 disc (Xbox, NTSC-U), already
   extracted, into the game_data folder.
   (Ainda nao ha launcher no Windows para ler a imagem .iso.)
   (There is no launcher on Windows yet to read the .iso image.)

3. De dois cliques em run.bat.  /  Double-click run.bat.
   Na primeira vez o Windows pode avisar que o programa e desconhecido
   (SmartScreen): "Mais informacoes" > "Executar assim mesmo".
   The first time, Windows may warn that the program is unknown (SmartScreen):
   "More info" > "Run anyway".

API grafica (Vulkan ou OpenGL), resolucao, proporcao (4:3, 16:9, 21:9), 30/60
quadros, tela cheia e vsync: abra o run.bat no Bloco de Notas e edite as
linhas do comeco.
Graphics API (Vulkan or OpenGL), resolution, aspect ratio, 30/60 fps,
fullscreen and vsync: open run.bat in Notepad and edit the lines at the top.

Controle: qualquer controle que o Windows reconheca (Xbox, PlayStation...),
ou o teclado.  /  Pad: any controller Windows recognises, or the keyboard.

Requisitos / Requirements: Windows 10 ou 11, 64 bits; placa de video com
driver Vulkan ou OpenGL 3.3 (NVIDIA, AMD ou Intel atualizados).
Windows 10 or 11, 64-bit; a GPU with a Vulkan or OpenGL 3.3 driver.


Onde ficam as coisas / Where things are
---------------------------------------

  save_data\   saves do jogo / game saves
  logs\        um arquivo por partida / one file per session  (run-*.log)

Se algo der errado, envie o arquivo mais recente de logs\ junto com o relato.
If something goes wrong, send the newest file in logs\ with your report.


Projeto de fas, sem vinculo com Bandai Namco, Toei Animation ou Microsoft.
Fan project, not affiliated with Bandai Namco, Toei Animation or Microsoft.
Nunca compartilhe arquivos do jogo. / Never share game files.
Licencas de terceiros / third-party licenses: licenses\
