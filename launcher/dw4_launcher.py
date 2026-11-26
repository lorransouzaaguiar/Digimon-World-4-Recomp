#!/usr/bin/env python3
"""Launcher do Digimon World 4 - Recomp (Qt Quick, PySide6).

    launcher.sh                 abre o launcher
    launcher.sh --play          inicia o jogo direto, com as opcoes salvas
    launcher.sh --selftest      carrega a interface fora da tela e sai ("selftest ok")
    launcher.sh --wine          (desenvolvimento) inicia o build de Windows no Wine:
                                build-win/dw4_recomp.exe, de scripts/build_win.sh
    launcher.sh --screenshot <pagina> <arquivo.png> [--lang pt|en] [--size LxA] [--defaults]
                                (documentacao/testes; nao grava a configuracao)

Funciona na raiz do repositorio (executavel em build/) e dentro de um pacote
de release (executavel ao lado). Tudo o que ele grava fica na pasta do
projeto/pacote: config/launcher.json, logs/, backups/, save_data/.

Ideia e divisao de telas inspiradas no launcher de hayatog/iss_pc; codigo proprio.
"""
import json
import locale
import os
import platform
import re
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))    # o Python embutido do Windows nao poe a pasta do script
import updater  # noqa: E402

if "--install-update" in sys.argv[1:]:
    # Ajudante de instalacao do Windows (updater.py): roda do pacote novo, numa
    # pasta de preparo, e nao abre janela. Antes do Qt de proposito.
    sys.exit(updater.run_install_helper(sys.argv))

from PySide6.QtCore import (Property, QObject, QProcess, QProcessEnvironment, Qt, QTimer, QUrl,
                            Signal, Slot)
from PySide6.QtGui import QDesktopServices, QGuiApplication, QIcon
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickWindow  # noqa: F401  (registra o tipo da janela)

HERE = Path(__file__).resolve().parent          # no pacote congelado: pasta de dados do PyInstaller
ROOT = HERE.parent
FROZEN = bool(getattr(sys, "frozen", False))

WIN = os.name == "nt"
# Pacote de release: congelado pelo PyInstaller (Linux) ou Python embutido
# iniciado pelo DigimonWorld4-Recomp.exe (Windows, que define DW4_PACKAGED).
PACKAGED = FROZEN or bool(os.environ.get("DW4_PACKAGED"))
EXE_NAME = "dw4_recomp.exe" if WIN else "dw4_recomp"


def _user_data_dir() -> Path:
    if WIN:
        return Path(os.environ.get("APPDATA") or Path.home() / "AppData" / "Roaming") / "dw4-recomp"
    return Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "dw4-recomp"


# APP: onde estao o executavel do jogo e lib/ (so leitura dentro de um AppImage).
# DATA: tudo o que se grava (opcoes, logs, saves, backups, game_data/).
#   - no repositorio e no pacote .tar.gz antigo: a propria pasta do projeto;
#   - no pacote: ~/.local/share/dw4-recomp (XDG_DATA_HOME) no Linux,
#     %APPDATA%\dw4-recomp no Windows.
APP = Path(os.environ.get("DW4_APP_DIR") or (Path(sys.executable).resolve().parent.parent / "game" if FROZEN else ROOT))
DATA = Path(os.environ.get("DW4_DATA_DIR") or (_user_data_dir() if PACKAGED else ROOT))
# --wine (so a partir do codigo): este launcher, nativo no Linux, inicia o .exe
# de Windows pelo Wine, para testar as opcoes de video no build de Windows antes
# de existir um launcher para Windows. Prefixo e saves proprios, para nao tocar
# no ~/.wine nem nos saves do build de Linux.
WINE = "--wine" in sys.argv[1:] and not FROZEN
WINE_EXE = ROOT / "build-win" / "dw4_recomp.exe"
WINE_PREFIX = ROOT / "tools" / "win-deps" / "wineprefix"

GAME_FILES = ["default.xbe", "area.afs", "ndmw.afs", "ndmw.igb"]
TITLE_ID = "42410002"
KEEP_LOGS = 10

DEFAULTS = {
    "game_dir": "",          # vazio = <raiz>/game_data
    "lang": "auto",          # auto | pt | en
    "fullscreen": False,
    "display": "",           # monitor da janela do jogo: nome do Qt (QScreen.name); "" = o principal
    "vsync": True,
    "renderer": "vulkan",    # vulkan | opengl  (API grafica; "gpu" antigo = opengl)
    "res": 480,              # altura da imagem: 480 (original) | 720 | 1080 | 1440 | 2160
    "aspect": "4:3",         # 4:3 (original) | 16:9 | 21:9
    "fps": 30,               # limite de quadros: 30 (como rodava no Xbox) | 60
    "parallel": False,       # jogo e GPU emulada em paralelo (RECOMP_KICK_AHEAD); experimental
    "keyboard": True,
    "pads": 4,               # portas de controle que o jogo enxerga, 1..4 (RECOMP_USB_PADS)
    "keymap": {},            # teclas trocadas do teclado-controle: {comando: tecla}; vazio = padrao
    "quit_keys": "ctrl+q",   # atalho de teclado que fecha o jogo ("" = desligado); src/hooks/quit_hotkey.c
    "quit_pad": "back+start",  # botoes do controle, segurados 1 s ("" = desligado)
    "osd": False,            # mostrador de FPS/CPU/GPU ja visivel ao abrir o jogo (src/hooks/osd.c)
    "osd_keys": "f2",        # tecla(s) que ligam/desligam o mostrador ("" = nenhuma)
    "osd_pad": "l3+r3",      # botoes do controle que ligam/desligam ("" = nenhum)
    "check_updates": True,   # consulta o GitHub ao abrir (launcher/updater.py)
}

# Atalho de fechar: nomes que o jogo entende (quit_hotkey.c) e como aparecem na tela.
KEY_NAMES = {Qt.Key_Control: "ctrl", Qt.Key_Shift: "shift", Qt.Key_Alt: "alt", Qt.Key_Escape: "esc",
             Qt.Key_Tab: "tab", Qt.Key_Space: "space", Qt.Key_Return: "enter", Qt.Key_Enter: "enter",
             Qt.Key_Backspace: "backspace", Qt.Key_Up: "up", Qt.Key_Down: "down", Qt.Key_Left: "left",
             Qt.Key_Right: "right", Qt.Key_Home: "home", Qt.Key_End: "end", Qt.Key_PageUp: "pgup",
             Qt.Key_PageDown: "pgdn", Qt.Key_Insert: "ins", Qt.Key_Delete: "del", Qt.Key_Pause: "pause"}
KEY_NAMES = {int(k): v for k, v in KEY_NAMES.items()}
KEY_LABELS = {"ctrl": "Ctrl", "shift": "Shift", "alt": "Alt", "esc": "Esc", "tab": "Tab", "space": "Space",
              "enter": "Enter", "backspace": "Backspace", "up": "↑", "down": "↓", "left": "←", "right": "→",
              "home": "Home", "end": "End", "pgup": "Page Up", "pgdn": "Page Down", "ins": "Insert",
              "del": "Delete", "pause": "Pause"}
KEY_MODS = ("ctrl", "shift", "alt")
QUIT_MAX = 4                 # o jogo aceita ate 4 teclas / botoes por atalho
PAD_BUTTONS = [("back", "Back"), ("start", "Start"), ("a", "A"), ("b", "B"), ("x", "X"), ("y", "Y"),
               ("lb", "LB"), ("rb", "RB"), ("lt", "LT"), ("rt", "RT"), ("l3", "L3"), ("r3", "R3")]
# Teclado como controle 1: comando do Xbox (nome do toolkit, input_map.c), como aparece e a tecla padrao.
KEYMAP_ROWS = [("start", "START", "enter"), ("back", "BACK", "backspace"),
               ("a", "A", "z"), ("b", "B", "x"), ("x", "X", "c"), ("y", "Y", "v"),
               ("white", "White", "q"), ("black", "Black", "e"),
               ("left_trigger", "L trigger", "1"), ("right_trigger", "R trigger", "3"),
               ("left_thumb", "L3", "shift"), ("right_thumb", "R3", "ctrl"),
               ("dpad_up", "D-pad ↑", "up"), ("dpad_down", "D-pad ↓", "down"),
               ("dpad_left", "D-pad ←", "left"), ("dpad_right", "D-pad →", "right"),
               ("left_stick_up", "L stick ↑", "w"), ("left_stick_down", "L stick ↓", "s"),
               ("left_stick_left", "L stick ←", "a"), ("left_stick_right", "L stick →", "d"),
               ("right_stick_up", "R stick ↑", "i"), ("right_stick_down", "R stick ↓", "k"),
               ("right_stick_left", "R stick ←", "j"), ("right_stick_right", "R stick →", "l")]
KEYMAP_DEFAULT = {c: k for c, _, k in KEYMAP_ROWS}
TOOLKIT_KEY = {"pgup": "pageup", "pgdn": "pagedown", "ins": "insert", "del": "delete"}     # nomes do toolkit
COMBO_KEYS = ("quit_keys", "osd_keys")      # opcoes que guardam um atalho de teclado
COMBO_PADS = ("quit_pad", "osd_pad")        # ... e de controle
UPDATE_EVERY = 6 * 3600      # consulta automatica: no maximo uma a cada 6 h


def key_valid(name: str) -> bool:
    return (name in KEY_LABELS or (len(name) == 1 and name.isascii() and name.isalnum() and name == name.lower())
            or bool(re.fullmatch(r"f([1-9]|1[0-2])", name)))


def key_label(name: str) -> str:
    return KEY_LABELS.get(name) or name.upper()


def clean_combo(text, valid) -> str:
    """"Ctrl+Q" gravado a mao -> "ctrl+q"; "" se houver nome que o jogo nao entende."""
    parts = [x.strip().lower() for x in str(text or "").split("+") if x.strip()]
    parts = list(dict.fromkeys(parts))
    return "+".join(parts) if parts and len(parts) <= QUIT_MAX and all(valid(x) for x in parts) else ""

STR = {
    "pt": {
        "missing_short": "Faltam os arquivos do jogo. Escolha a imagem do seu disco ou a pasta acima.",
        "play": "Jogar", "graphics": "Gráficos", "controls": "Controles", "saves": "Saves e dados",
        "support": "Suporte",
        "game_folder": "Arquivos do jogo", "choose": "Escolher pasta…", "use_default": "Usar a pasta padrão",
        "choose_iso": "Escolher imagem do disco (.iso)…",
        "iso_working": "Copiando os arquivos do jogo…", "iso_done": "Arquivos do jogo copiados. Pronto para jogar.",
        "iso_bad": "Este arquivo não é uma imagem de disco de Xbox.",
        "iso_missing": "Esta imagem não é de Digimon World 4: faltam ",
        "iso_io": "Não foi possível copiar: ",
        "gpu": "Placa de vídeo", "version": "Versão", "status": "Situação",
        "ready": "Pronto para jogar", "running": "O jogo está aberto…",
        "no_exe": "Executável não encontrado. Compile o projeto (veja o README).",
        "missing": "Faltam arquivos do jogo nesta pasta: ",
        "need_files": "O jogo não vem junto. Aponte para a imagem (.iso) do seu próprio disco de Digimon World 4 "
                      "(Xbox, NTSC-U) ou para uma pasta com o disco já extraído.",
        "btn_play": "JOGAR", "btn_running": "EM EXECUÇÃO",
        "ended_ok": "O jogo foi fechado.",
        "ended_crash": "O jogo fechou com erro. O log foi guardado em: ",
        "display": "Monitor", "display_auto": "Automático (monitor principal)",
        "display_d": "Em qual monitor o jogo abre, em janela ou em tela cheia. Se o monitor escolhido não "
                     "estiver ligado, o jogo abre no principal.",
        "display_gone": "(desligado)",
        "window": "Janela", "fullscreen": "Tela cheia", "fullscreen_d": "Tela cheia sem borda, na resolução da área de trabalho.",
        "window_d": "Fora da tela cheia, a janela abre do tamanho da resolução interna escolhida acima (em 480p e "
                    "720p, no dobro, para não ficar pequena). Se não couber no monitor, é reduzida mantendo a forma.",
        "vsync": "Sincronização vertical", "vsync_d": "Apresenta os quadros no ritmo do monitor. Desligue só para testes.",
        "perf": "Desempenho", "parallel": "Jogo e placa de vídeo em paralelo (experimental)",
        "parallel_d": "Ligue se o jogo não segura a taxa de quadros escolhida (cai abaixo de 30 ou de 60 fps em "
                      "áreas cheias) e o seu processador tem 4 núcleos ou mais: a lógica do jogo e o desenho "
                      "passam a rodar ao mesmo tempo em dois núcleos, em vez de se revezarem. "
                      "Deixe desligado se o jogo já roda liso (não muda nada, como num PC forte) ou se o "
                      "processador tem só 2 núcleos (não há núcleo sobrando para aproveitar). "
                      "Se algo piscar ou sumir na tela com isto ligado, desligue.",
        "image": "Imagem",
        "res": "Resolução interna", "res_orig": "480p (original)",
        "res_d": "Altura em que o jogo é desenhado. Use a do seu monitor para a imagem mais nítida; "
                 "valores altos pedem mais da placa de vídeo.",
        "aspect": "Proporção", "aspect_orig": "4:3 (original)",
        "aspect_d": "Em 16:9 ou 21:9 o cenário aparece mais largo, sem esticar. O painel do jogador e o "
                    "minimapa vão para os cantos da tela; menus, diálogos e vídeos continuam em 4:3 no meio.",
        "fps": "Limite de quadros", "fps_30": "30 fps (original)", "fps_60": "60 fps (2× mais rápido)",
        "fps_d": "No Xbox o jogo rodava a 30 quadros por segundo. A velocidade dele acompanha os quadros: "
                 "em 60 fps a imagem fica mais fluida, mas tudo (movimento, combate, textos) anda duas vezes "
                 "mais rápido que no console.",
        "renderer": "API gráfica", "r_vk": "Vulkan — padrão", "r_gl": "OpenGL 3.3",
       
        "renderer_d": "É a forma como o jogo fala com a placa de vídeo. Vulkan e OpenGL desenham a mesma imagem; "
                      "o Vulkan é o mais rápido, principalmente em resolução alta.",
        "api_vk": "Vulkan (padrão) — o mais rápido: o quadro pronto vai direto da placa de vídeo para a janela. "
                  "Em 1440p e 4K é o que segura os 60 fps. Precisa de um driver com Vulkan.",
        "api_gl": "OpenGL 3.3 — a mais antiga e compatível; funciona em praticamente qualquer placa e driver. "
                  "Mais lenta em resolução alta. Jogando, o defeito conhecido do chão que escurece apareceu menos com ela, "
                  "e a imagem ficou mais estável: vale trocar se isso incomodar ou se o Vulkan der problema.",
        "api_note": "Com Vulkan o jogo usa pouco processador (bem menos de um núcleo em cenas leves) e deixa o "
                    "trabalho pesado para a placa de vídeo; medido numa RTX 5070, onde 4K roda a 60 fps. "
                    "O OpenGL traz cada quadro de volta pela memória, o que custa quadros de 1440p para cima; "
                    "em compensação, o defeito conhecido do chão que escurece aparece menos com ele. "
                    "Se a API escolhida não estiver disponível, o jogo abre com a outra.",
        "vk_missing": "Vulkan não foi encontrado neste sistema: o jogo vai abrir com OpenGL.",
        "keyboard": "Teclado como controle 1", "keyboard_d": "Um controle conectado continua funcionando junto.",
        "pad_note": "Qualquer controle reconhecido como Xbox/XInput funciona sem configuração.",
        "key": "Tecla", "xbox": "Controle do Xbox",
        "pads": "Portas de controle",
        "pads_d": "Quantos controles o jogo enxerga, como as 4 portas do Xbox: é o que permite jogar com até 4 "
                  "jogadores. Os controles ocupam as portas na ordem em que o sistema os reconhece; o teclado "
                  "conta sempre como o controle 1. Se o jogo se comportar de forma estranha com portas vazias, reduza.",
        "keymap_d": "Clique numa tecla e aperte a nova. Se ela já estiver em outro comando, os dois trocam.",
        "keymap_reset": "Restaurar o padrão",
        "quit": "Fechar o jogo", "quit_d": "Em tela cheia não há botão de fechar: use um destes atalhos durante o jogo. "
                                           "O que não foi salvo no jogo é perdido, como ao fechar a janela.",
        "quit_keys": "Atalho do teclado", "quit_keys_d": "As teclas apertadas ao mesmo tempo fecham o jogo na hora.",
        "quit_set": "Definir…", "quit_clear": "Desligar", "quit_off": "Desligado",
        "quit_press": "Aperte a tecla ou a combinação (até 4 teclas) e solte…",
        "quit_pad": "Atalho do controle",
        "quit_pad_d": "Marque de 2 a 4 botões. Segurados juntos por 1 segundo, em qualquer controle, fecham o jogo. "
                      "LB e RB são os botões Black e White do Xbox; LT e RT, os gatilhos.",
        "quit_pad_one": "Um botão sozinho não vale como atalho: marque pelo menos dois.",
        "osd": "Mostrador de desempenho",
        "osd_d": "Mostra no canto da tela, durante o jogo, os quadros por segundo (FPS), o uso do processador (CPU) "
                 "e o da placa de vídeo (GPU). CPU: média de todos os núcleos do computador, como no monitor do "
                 "sistema (inclui os outros programas). GPU: ocupação da placa inteira, não só do jogo; aparece \"--\" quando o sistema não informa.",
        "osd_start": "Já mostrar ao abrir o jogo",
        "osd_start_d": "Desligado, o mostrador só aparece quando você usar o atalho abaixo.",
        "osd_keys": "Tecla de mostrar/esconder", "osd_keys_d": "Aperte durante o jogo para mostrar ou esconder.",
        "osd_pad": "Botões de mostrar/esconder",
        "osd_pad_d": "Marque de 2 a 4 botões. Apertados juntos, em qualquer controle, mostram ou escondem.",
        "updates": "Atualizações", "upd_check": "Procurar atualização", "upd_install": "Atualizar agora",
        "upd_page": "Abrir a página de download", "upd_notes": "Novidades da versão", "upd_cancel": "Cancelar",
        "upd_auto": "Procurar atualizações ao abrir",
        "upd_auto_d": "Uma consulta ao GitHub quando o launcher abre, no máximo a cada 6 horas. "
                      "Nada é instalado sem você pedir.",
        "upd_checking": "Procurando atualização…", "upd_current": "Você já está na versão mais recente (%s).",
        "upd_available": "Nova versão disponível: %s", "upd_dev": "Versão mais recente publicada: %s.",
        "upd_downloading": "Baixando %s…", "upd_installing": "Instalando %s: o launcher vai fechar e abrir de novo.",
        "upd_manual_d": "Esta instalação (pacote .tar.gz ou código-fonte) não se atualiza sozinha: "
                        "baixe o pacote novo na página do projeto. Saves e opções são mantidos.",
        "upd_self_d": "O launcher baixa o pacote, confere e se reabre na versão nova. Saves e opções são mantidos.",
        "upd_running": "Feche o jogo antes de atualizar.", "upd_cancelled": "Atualização cancelada.",
        "upd_err_net": "Não foi possível falar com o GitHub. Confira a conexão com a internet.",
        "upd_err_rate": "O GitHub limitou as consultas deste endereço por agora. Tente de novo mais tarde.",
        "upd_err_http": "O GitHub respondeu com erro. Tente de novo mais tarde.",
        "upd_err_bad": "Resposta inesperada do GitHub; nada foi instalado.",
        "upd_err_hash": "O arquivo baixado não confere com o do release; nada foi instalado. Tente de novo.",
        "upd_err_write": "Não foi possível gravar a atualização: ",
        "upd_err_noasset": "Esta versão não tem pacote para a sua instalação. Baixe pela página do projeto.",
        "save_folder": "Pasta dos saves", "open_folder": "Abrir pasta", "backup": "Fazer backup",
        "export": "Exportar .zip…", "import": "Importar .zip…", "no_saves": "Nenhum save ainda.",
        "save_list": "Saves encontrados", "backup_done": "Backup gravado em: ",
        "export_done": "Saves exportados para: ", "import_done": "Saves importados. O backup dos anteriores está em: ",
        "import_bad": "Este .zip não parece um backup de saves deste launcher.",
        "not_while_running": "Feche o jogo antes de mexer nos saves.",
        "known": "Problemas conhecidos",
        "known_1": "Pular o vídeo de abertura com START pode, raramente, fechar o jogo. Se acontecer, abra de novo.",
        "known_2": "Reflexos e brilhos gerados automaticamente não estão implementados; alguns objetos podem parecer chapados.",
        "known_3": "Em algumas áreas o chão escurece por alguns quadros e volta. É só visual; a causa ainda não foi achada. "
                   "Acontece menos com a API gráfica OpenGL (Gráficos).",
        "known_4": "Testado em uma única máquina (CachyOS, NVIDIA RTX 5070), onde roda a 60 fps em 4K. Outras placas "
                   "(AMD, Intel), processadores mais fracos e outros sistemas ainda não foram testados.",
        "slow": "Se o jogo estiver lento",
        "slow_1": "Use a API gráfica Vulkan (Gráficos). Com OpenGL cada quadro passa pela memória, o que pesa de 1440p para cima.",
        "slow_2": "Baixe a resolução interna (Gráficos → Imagem). É o que mais alivia a placa de vídeo.",
        "slow_3": "Em processador com 4 núcleos ou mais, ligue \"Jogo e placa de vídeo em paralelo\" (Gráficos → Desempenho).",
        "slow_4": "Limite de 30 fps é a velocidade original do jogo; em 60 fps tudo anda duas vezes mais rápido e pesa o dobro.",
        "slow_5": "Continua lento? Jogue um pouco na área lenta, feche o jogo e gere o relatório abaixo: o log anota a "
                  "cada 5 segundos os quadros por segundo e onde o tempo foi gasto.",
        "logs": "Logs", "open_logs": "Abrir pasta de logs", "report": "Gerar relatório",
        "report_d": "Junta versão, sistema, processador, placa de vídeo, opções, um resumo dos quadros por segundo e "
                    "o fim do último log em um arquivo de texto, "
                    "sem o nome da sua pasta pessoal. Nenhum arquivo do jogo entra.",
        "report_done": "Relatório gravado em: ", "no_log": "Ainda não há log: jogue uma vez pelo launcher.",
        "language": "Idioma do launcher", "l_auto": "Automático", "freeze_tip":
            "Se o jogo travar, espere uns 10 segundos antes de fechar: o log registra onde ele parou.",
    },
    "en": {
        "missing_short": "The game files are missing. Choose your disc image or folder above.",
        "play": "Play", "graphics": "Graphics", "controls": "Controls", "saves": "Saves & data",
        "support": "Support",
        "game_folder": "Game files", "choose": "Choose folder…", "use_default": "Use the default folder",
        "choose_iso": "Choose disc image (.iso)…",
        "iso_working": "Copying the game files…", "iso_done": "Game files copied. Ready to play.",
        "iso_bad": "This file is not an Xbox disc image.",
        "iso_missing": "This image is not Digimon World 4: missing ",
        "iso_io": "Could not copy: ",
        "gpu": "Graphics card", "version": "Version", "status": "Status",
        "ready": "Ready to play", "running": "The game is running…",
        "no_exe": "Executable not found. Build the project (see the README).",
        "missing": "Game files missing from this folder: ",
        "need_files": "The game is not included. Point to the image (.iso) of your own Digimon World 4 disc "
                      "(Xbox, NTSC-U) or to a folder with the disc already extracted.",
        "btn_play": "PLAY", "btn_running": "RUNNING",
        "ended_ok": "The game was closed.",
        "ended_crash": "The game closed with an error. The log was kept at: ",
        "display": "Display", "display_auto": "Automatic (primary display)",
        "display_d": "Which display the game opens on, windowed or fullscreen. If the chosen display is not "
                     "connected, the game opens on the primary one.",
        "display_gone": "(disconnected)",
        "window": "Window", "fullscreen": "Fullscreen", "fullscreen_d": "Borderless fullscreen at the desktop resolution.",
        "window_d": "Outside fullscreen, the window opens at the internal resolution chosen above (doubled at 480p "
                    "and 720p, so it is not tiny). If it does not fit the display it is shrunk, keeping its shape.",
        "vsync": "Vertical sync", "vsync_d": "Presents frames at the display's pace. Turn off for testing only.",
        "perf": "Performance", "parallel": "Game and graphics card in parallel (experimental)",
        "parallel_d": "Turn on if the game cannot hold the frame rate you chose (drops below 30 or 60 fps in "
                      "busy areas) and your processor has 4 or more cores: game logic and drawing then run "
                      "at the same time on two cores instead of taking turns. "
                      "Leave off if the game already runs smoothly (it changes nothing, as on a fast PC) or "
                      "if the processor has only 2 cores (there is no spare core to use). "
                      "If anything flickers or goes missing on screen with this on, turn it off.",
        "image": "Picture",
        "res": "Internal resolution", "res_orig": "480p (original)",
        "res_d": "The height the game is drawn at. Use your display's for the sharpest picture; "
                 "high values ask more of the graphics card.",
        "aspect": "Aspect ratio", "aspect_orig": "4:3 (original)",
        "aspect_d": "At 16:9 or 21:9 the scenery gets wider without stretching. The player panel and the "
                    "minimap move to the screen corners; menus, dialogue and movies stay 4:3 in the middle.",
        "fps": "Frame rate limit", "fps_30": "30 fps (original)", "fps_60": "60 fps (2× faster)",
        "fps_d": "On the Xbox the game ran at 30 frames per second. Its speed follows the frames: at 60 fps "
                 "the picture is smoother, but everything (movement, combat, text) goes twice as fast as "
                 "on the console.",
        "renderer": "Graphics API", "r_vk": "Vulkan — default", "r_gl": "OpenGL 3.3",
       
        "renderer_d": "This is how the game talks to your graphics card. Vulkan and OpenGL draw the same picture; "
                      "Vulkan is the faster one, mostly at high resolutions.",
        "api_vk": "Vulkan (default) — the faster one: the finished frame goes straight from the graphics card to "
                  "the window. At 1440p and 4K this is what holds 60 fps. Needs a driver with Vulkan.",
        "api_gl": "OpenGL 3.3 — the older, most compatible one; works on practically any card and driver. "
                  "Slower at high resolutions. In play, the known floor-darkening glitch showed up less with it and the "
                  "picture was steadier: worth switching if that bothers you or if Vulkan gives you trouble.",
        "api_note": "With Vulkan the game uses little processor (well under one core in light scenes) and leaves "
                    "the heavy work to the graphics card; measured on an RTX 5070, where 4K runs at 60 fps. "
                    "OpenGL copies every frame back through memory, which costs frames at 1440p and above; "
                    "on the other hand, the known floor-darkening glitch shows up less with it. "
                    "If the chosen API is unavailable, the game starts with the other.",
        "vk_missing": "Vulkan was not found on this system: the game will start with OpenGL.",
        "keyboard": "Keyboard as controller 1", "keyboard_d": "A connected controller keeps working alongside it.",
        "pad_note": "Any controller the system reports as Xbox/XInput works with no setup.",
        "key": "Key", "xbox": "Xbox control",
        "pads": "Controller ports",
        "pads_d": "How many controllers the game sees, like the Xbox's 4 ports: this is what allows up to 4 "
                  "players. Controllers take the ports in the order the system reports them; the keyboard always "
                  "counts as controller 1. If the game misbehaves with empty ports, lower it.",
        "keymap_d": "Click a key and press the new one. If it is already on another control, the two swap.",
        "keymap_reset": "Restore defaults",
        "quit": "Quit the game", "quit_d": "Fullscreen has no close button: use one of these shortcuts while playing. "
                                           "Anything not saved in the game is lost, as when closing the window.",
        "quit_keys": "Keyboard shortcut", "quit_keys_d": "The keys pressed together quit the game at once.",
        "quit_set": "Set…", "quit_clear": "Turn off", "quit_off": "Off",
        "quit_press": "Press the key or combination (up to 4 keys) and release…",
        "quit_pad": "Controller shortcut",
        "quit_pad_d": "Tick 2 to 4 buttons. Held together for 1 second, on any controller, they quit the game. "
                      "LB and RB are the Xbox Black and White buttons; LT and RT, the triggers.",
        "quit_pad_one": "A single button is not accepted as a shortcut: tick at least two.",
        "osd": "Performance overlay",
        "osd_d": "Shows, in a corner of the screen while playing, the frames per second (FPS), processor use (CPU) "
                 "and graphics card use (GPU). CPU: average of all the computer's cores, as in the system "
                 "monitor (other programs included). GPU: load of the whole card, not only the game; \"--\" when the system does not report it.",
        "osd_start": "Already showing when the game starts",
        "osd_start_d": "When off, the overlay only appears when you use the shortcut below.",
        "osd_keys": "Show/hide key", "osd_keys_d": "Press while playing to show or hide it.",
        "osd_pad": "Show/hide buttons",
        "osd_pad_d": "Tick 2 to 4 buttons. Pressed together, on any controller, they show or hide it.",
        "updates": "Updates", "upd_check": "Check for updates", "upd_install": "Update now",
        "upd_page": "Open the download page", "upd_notes": "What's new", "upd_cancel": "Cancel",
        "upd_auto": "Check for updates on start",
        "upd_auto_d": "One request to GitHub when the launcher opens, at most every 6 hours. "
                      "Nothing is installed unless you ask.",
        "upd_checking": "Checking for updates…", "upd_current": "You already have the latest version (%s).",
        "upd_available": "New version available: %s", "upd_dev": "Latest published version: %s.",
        "upd_downloading": "Downloading %s…", "upd_installing": "Installing %s: the launcher will close and open again.",
        "upd_manual_d": "This install (.tar.gz package or source code) does not update itself: "
                        "download the new package from the project page. Saves and options are kept.",
        "upd_self_d": "The launcher downloads the package, verifies it and reopens on the new version. "
                      "Saves and options are kept.",
        "upd_running": "Close the game before updating.", "upd_cancelled": "Update cancelled.",
        "upd_err_net": "Could not reach GitHub. Check your internet connection.",
        "upd_err_rate": "GitHub is limiting requests from this address for now. Try again later.",
        "upd_err_http": "GitHub answered with an error. Try again later.",
        "upd_err_bad": "Unexpected answer from GitHub; nothing was installed.",
        "upd_err_hash": "The downloaded file does not match the release; nothing was installed. Try again.",
        "upd_err_write": "Could not write the update: ",
        "upd_err_noasset": "This version has no package for your install. Download it from the project page.",
        "save_folder": "Save folder", "open_folder": "Open folder", "backup": "Back up",
        "export": "Export .zip…", "import": "Import .zip…", "no_saves": "No saves yet.",
        "save_list": "Saves found", "backup_done": "Backup written to: ",
        "export_done": "Saves exported to: ", "import_done": "Saves imported. The previous ones were backed up to: ",
        "import_bad": "This .zip does not look like a save backup from this launcher.",
        "not_while_running": "Close the game before touching the saves.",
        "known": "Known issues",
        "known_1": "Skipping the intro movie with START can, rarely, make the game quit. If it happens, start it again.",
        "known_2": "Automatically generated reflections and shine are not implemented; some objects may look flat.",
        "known_3": "In some areas the floor darkens for a few frames and comes back. It is only visual; the cause has not been found yet. "
                   "It happens less with the OpenGL graphics API (Graphics).",
        "known_4": "Tested on one machine (CachyOS, NVIDIA RTX 5070), where it runs at 60 fps in 4K. Other cards "
                   "(AMD, Intel), slower processors and other systems are untested.",
        "slow": "If the game is slow",
        "slow_1": "Use the Vulkan graphics API (Graphics). With OpenGL every frame goes through memory, which is heavy from 1440p up.",
        "slow_2": "Lower the internal resolution (Graphics → Picture). It is what relieves the graphics card the most.",
        "slow_3": "On a processor with 4 or more cores, turn on \"Game and graphics card in parallel\" (Graphics → Performance).",
        "slow_4": "The 30 fps limit is the game's original speed; at 60 fps everything moves twice as fast and costs twice as much.",
        "slow_5": "Still slow? Play a little in the slow area, close the game and create the report below: every 5 "
                  "seconds the log records the frames per second and where the time went.",
        "logs": "Logs", "open_logs": "Open log folder", "report": "Create report",
        "report_d": "Collects version, system, processor, graphics card, options, a frames-per-second summary and "
                    "the end of the last log into a text file, "
                    "without your home folder's name. No game file is included.",
        "report_done": "Report written to: ", "no_log": "No log yet: play once through the launcher.",
        "language": "Launcher language", "l_auto": "Automatic", "freeze_tip":
            "If the game freezes, wait about 10 seconds before closing: the log records where it stopped.",
    },
}

def find_ci(folder: Path, name: str):
    """Arquivo `name` em `folder`, sem diferenciar maiusculas (FATX)."""
    try:
        for e in folder.iterdir():
            if e.name.lower() == name:
                return e
    except OSError:
        pass
    return None


def scrub(text: str) -> str:
    home = str(Path.home())
    return text.replace(home, "~") if home and home != "/" else text


class Backend(QObject):
    changed = Signal()
    langChanged = Signal()
    runningChanged = Signal()
    message = Signal(str)

    def __init__(self):
        super().__init__()
        self.cfg_path = DATA / "config" / "launcher.json"
        self.cfg = dict(DEFAULTS)
        try:
            saved = json.loads(self.cfg_path.read_text())
            self.cfg.update({k: v for k, v in saved.items() if k in DEFAULTS})
        except (OSError, ValueError):
            pass
        if self.cfg["res"] not in (480, 720, 1080, 1440, 2160):
            self.cfg["res"] = 480
        if self.cfg["fps"] not in (60, 30):
            self.cfg["fps"] = 30
        if self.cfg["aspect"] == "16:10":       # deixou de ser oferecida (DW4R-2): a mais proxima
            self.cfg["aspect"] = "16:9"
        if self.cfg["aspect"] not in ("4:3", "16:9", "21:9"):
            self.cfg["aspect"] = "4:3"
        if self.cfg["renderer"] == "cpu":       # deixou de ser oferecido no launcher (DW4R-2)
            self.cfg["renderer"] = "vulkan"
        if self.cfg["renderer"] not in ("opengl", "vulkan"):
            self.cfg["renderer"] = "opengl"     # "gpu" das versoes ate 0.1.0
        for k in COMBO_KEYS:
            self.cfg[k] = clean_combo(self.cfg[k], key_valid)
        for k in COMBO_PADS:
            self.cfg[k] = clean_combo(self.cfg[k], dict(PAD_BUTTONS).__contains__)
        self.cfg["osd"] = bool(self.cfg["osd"])
        if not isinstance(self.cfg["display"], str):
            self.cfg["display"] = ""
        if self.cfg["pads"] not in (1, 2, 3, 4):
            self.cfg["pads"] = 4
        km = self.cfg["keymap"] if isinstance(self.cfg["keymap"], dict) else {}
        self.cfg["keymap"] = {c: k for c, k in km.items()
                              if c in KEYMAP_DEFAULT and isinstance(k, str) and key_valid(k) and k != KEYMAP_DEFAULT[c]}
        self.cfg["check_updates"] = bool(self.cfg["check_updates"])
        self._vulkan = self._detect_vulkan()
        self._upd_kind = updater.install_kind(FROZEN, PACKAGED, APP)
        self._upd = {"state": "idle", "tag": "", "page": updater.RELEASES_PAGE, "progress": 0.0, "err": ""}
        self._upd_busy = False
        self._upd_cancel = False
        self._updDone.connect(self._upd_finished)
        self._updProgress.connect(self._upd_set_progress)
        self._upd_load_cache()
        self.proc = None
        self.log_file = None
        self.last_log = None
        self._gpu = self._detect_gpu()

    # ── caminhos ────────────────────────────────────────────────────────────
    def exe(self):
        if WINE:
            return WINE_EXE if WINE_EXE.is_file() and shutil.which("wine") else None
        for p in (APP / EXE_NAME, APP / "build" / EXE_NAME):
            if p.is_file() and os.access(p, os.X_OK):
                return p
        return None

    def game_dir(self) -> Path:
        return Path(self.cfg["game_dir"]) if self.cfg["game_dir"] else DATA / "game_data"

    def save_dir(self) -> Path:
        return ROOT / "build-win" / "save_data" if WINE else DATA / "save_data"

    def command(self) -> list:
        """Programa e argumentos que iniciam o jogo."""
        cmd = [str(self.exe()), str(self.game_dir())]
        return ["wine"] + cmd if WINE else cmd

    def log_dir(self) -> Path:
        return DATA / "logs"

    def shot_dir(self) -> Path:
        """Capturas (F9) e caixa-preta de quadros (F12): no pacote ficam na pasta
        de dados do usuario; a partir do codigo, em build/shots como sempre."""
        return DATA / "shots" if PACKAGED else ROOT / "build" / "shots"

    def missing_files(self):
        d = self.game_dir()
        return [n for n in GAME_FILES if not find_ci(d, n)]

    # ── deteccao ────────────────────────────────────────────────────────────
    @staticmethod
    def _win_reg(key: str, value: str) -> str:
        """Um valor de texto do registro (HKLM); "" se nao existir."""
        try:
            import winreg
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, key) as k:
                return str(winreg.QueryValueEx(k, value)[0]).strip()
        except (OSError, ImportError):
            return ""

    @staticmethod
    def _detect_gpu() -> str:
        if WIN:
            # Adaptadores de video instalados; o "basico" da Microsoft so se nao houver outro.
            base = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
            names = [Backend._win_reg(base + "\\%04d" % i, "DriverDesc") for i in range(8)]
            names = [n for n in names if n]
            real = [n for n in names if "Microsoft" not in n]
            return (real or names or ["—"])[0]
        for cmd, rx in ((["glxinfo", "-B"], r"OpenGL renderer string:\s*(.+)"),
                        (["lspci"], r"(?:VGA compatible controller|3D controller):\s*(.+)")):
            try:
                out = subprocess.run(cmd, capture_output=True, text=True, timeout=4).stdout
            except (OSError, subprocess.SubprocessError):
                continue
            m = re.search(rx, out)
            if m:
                return m.group(1).strip()
        return "—"

    def _detect_vulkan(self) -> bool:
        """O renderizador Vulkan veio no pacote e o sistema tem o loader do Vulkan."""
        if WIN:                         # o renderizador fica dentro do executavel
            try:
                import ctypes
                ctypes.WinDLL("vulkan-1.dll")
                return self.exe() is not None
            except OSError:
                return False
        if not any((d / "libdw4_vk.so").is_file() for d in (APP, APP / "build")):
            return False
        try:
            import ctypes
            ctypes.CDLL("libvulkan.so.1")
            return True
        except OSError:
            return False

    def _save(self):
        self.cfg_path.parent.mkdir(parents=True, exist_ok=True)
        self.cfg_path.write_text(json.dumps(self.cfg, indent=2) + "\n")

    def _lang(self) -> str:
        if self.cfg["lang"] in STR:
            return self.cfg["lang"]
        loc = (os.environ.get("LC_ALL") or os.environ.get("LC_MESSAGES") or os.environ.get("LANG")
               or (locale.getlocale()[0] or ""))
        # No Windows o Python devolve "Portuguese_Brazil"; o Qt, "pt_BR" em todo sistema.
        loc = loc.lower()
        return "pt" if loc.startswith("pt") or loc.startswith("portuguese") else "en"

    # ── propriedades para o QML ─────────────────────────────────────────────
    @Property("QVariantMap", notify=langChanged)
    def t(self):
        return STR[self._lang()]

    @Property(str, constant=True)
    def gpu(self):
        return self._gpu

    @Property(bool, constant=True)
    def vulkanAvailable(self):
        return self._vulkan

    @Property(str, constant=True)
    def version(self):
        try:
            return (HERE / "VERSION").read_text().strip()
        except OSError:
            return "dev"

    @Property(str, notify=changed)
    def gameDir(self):
        return scrub(str(self.game_dir()))      # so para exibir

    @Property(str, notify=changed)
    def saveDir(self):
        return scrub(str(self.save_dir()))

    @Property(bool, notify=changed)
    def canPlay(self):
        return self.exe() is not None and not self.missing_files()

    @Property(str, notify=changed)
    def statusText(self):
        t = STR[self._lang()]
        if self.exe() is None:
            return t["no_exe"]
        miss = self.missing_files()
        if len(miss) == len(GAME_FILES):
            return t["missing_short"]
        if miss:
            return t["missing"] + ", ".join(miss)
        return t["ready"]

    @Property(bool, notify=runningChanged)
    def running(self):
        return self.proc is not None

    @Property("QVariantList", notify=changed)
    def saves(self):
        base = self.save_dir() / "UDATA" / TITLE_ID
        out = []
        if base.is_dir():
            for d in sorted(base.iterdir()):
                if not d.is_dir():
                    continue
                files = [f for f in d.rglob("*") if f.is_file()]
                if not files:
                    continue
                newest = max(f.stat().st_mtime for f in files)
                out.append({"name": d.name,
                            "when": time.strftime("%Y-%m-%d %H:%M", time.localtime(newest)),
                            "size": "%d KB" % (sum(f.stat().st_size for f in files) // 1024)})
        return out

    @Slot(str, result="QVariant")
    def opt(self, key):
        return self.cfg.get(key)

    @Slot(str, "QVariant")
    def setOpt(self, key, value):
        if key not in DEFAULTS or self.cfg.get(key) == value:
            return
        self.cfg[key] = value
        self._rev = getattr(self, "_rev", 0) + 1
        self._save()
        if key == "lang":
            self.langChanged.emit()
        self.changed.emit()

    @Slot(QUrl)
    def setGameDir(self, url):
        self.setOpt("game_dir", url.toLocalFile())

    # ── monitor ─────────────────────────────────────────────────────────────
    @staticmethod
    def _screens() -> list:
        app = QGuiApplication.instance()
        return list(app.screens()) if app else []

    @Slot(result="QVariantList")
    def displays(self):
        """Monitores ligados agora: [{name, label}], na ordem do sistema."""
        out = []
        for i, s in enumerate(self._screens()):
            g, r = s.geometry(), s.devicePixelRatio()
            model = " ".join(x for x in (s.manufacturer(), s.model()) if x).strip()
            out.append({"name": s.name(),
                        "label": "%d: %s (%d×%d)" % (i + 1, model if model and model != s.name()
                                                     else s.name(), round(g.width() * r), round(g.height() * r))})
        return out

    def _display_env(self) -> dict:
        """As pistas que o jogo aceita para achar o monitor (toolkit, patch 4.19).

        O Qt daqui e a SDL do jogo nao combinam em nome, em numeracao nem na origem
        das coordenadas (X11: canto do conjunto; Windows e Wine: canto do monitor
        principal; Wayland nem tem principal). O que os dois veem igual e a
        DISPOSICAO dos monitores, entao vai: a posicao na ordem esquerda->direita,
        cima->baixo, e um ponto medido a partir do canto do retangulo que envolve
        todos os monitores. Nome e indice vao juntos, para quando casarem."""
        name, screens = self.cfg["display"], self._screens()
        geo = [s.geometry() for s in screens]
        for i, s in enumerate(screens):
            if name and s.name() == name:
                g = geo[i]
                order = sorted(range(len(geo)), key=lambda j: (geo[j].x(), geo[j].y(), j))
                x0, y0 = min(r.x() for r in geo), min(r.y() for r in geo)
                return {"RECOMP_DISPLAY_NAME": name, "RECOMP_DISPLAY": str(i),
                        "RECOMP_DISPLAY_RANK": "%d/%d" % (order.index(i), len(geo)),
                        "RECOMP_DISPLAY_AT": "%d,%d" % (g.x() + g.width() // 2 - x0, g.y() + g.height() // 2 - y0)}
        return {}                       # automatico, ou o monitor escolhido nao esta ligado

    # ── atalho de fechar o jogo ─────────────────────────────────────────────
    @Slot(int, result=str)
    def keyName(self, key):
        """Tecla do Qt -> nome que o jogo entende; "" se nao serve para o atalho."""
        if key in KEY_NAMES:
            return KEY_NAMES[key]
        if int(Qt.Key_A) <= key <= int(Qt.Key_Z) or int(Qt.Key_0) <= key <= int(Qt.Key_9):
            return chr(key).lower()
        if int(Qt.Key_F1) <= key <= int(Qt.Key_F12):
            return "f%d" % (key - int(Qt.Key_F1) + 1)
        return ""

    @Slot("QVariantList", result=str)
    def keysLabel(self, names):
        return " + ".join(key_label(str(n)) for n in names)

    # ── teclas do teclado-controle ──────────────────────────────────────────
    def _keymap(self) -> dict:
        return dict(KEYMAP_DEFAULT, **self.cfg["keymap"])

    @Slot(result="QVariantList")
    def keymapRows(self):
        km = self._keymap()
        return [{"control": c, "label": lb, "key": key_label(km[c]), "changed": km[c] != KEYMAP_DEFAULT[c]}
                for c, lb, _ in KEYMAP_ROWS]

    @Slot(str, str)
    def setKeymap(self, control, name):
        """Poe a tecla `name` no comando; se ela ja era de outro, os dois trocam."""
        if control not in KEYMAP_DEFAULT or not key_valid(name):
            return
        km = self._keymap()
        for other, k in km.items():
            if k == name and other != control:
                km[other] = km[control]
        km[control] = name
        self.setOpt("keymap", {c: k for c, k in km.items() if k != KEYMAP_DEFAULT[c]})

    @Slot()
    def resetKeymap(self):
        self.setOpt("keymap", {})

    @Slot(str, "QVariantList")
    def setComboKeys(self, opt, names):
        if opt not in COMBO_KEYS:
            return
        names = [str(n) for n in names if key_valid(str(n))]
        names = list(dict.fromkeys(names))[:QUIT_MAX]
        names.sort(key=lambda n: KEY_MODS.index(n) if n in KEY_MODS else len(KEY_MODS))     # Ctrl + Q, nao Q + Ctrl
        self.setOpt(opt, "+".join(names))

    @Property(int, notify=changed)
    def rev(self):
        """Muda a cada opcao gravada: o QML refaz o que le por funcao (comboLabel...)."""
        return getattr(self, "_rev", 0)

    @Slot(str, result=str)
    def comboLabel(self, opt):
        k = self.cfg.get(opt, "") if opt in COMBO_KEYS else ""
        return self.keysLabel(k.split("+")) if k else STR[self._lang()]["quit_off"]

    @Slot(str, result="QVariantList")
    def padButtons(self, opt):
        on = self.cfg.get(opt, "").split("+") if opt in COMBO_PADS else []
        return [{"name": n, "label": lb, "on": n in on} for n, lb in PAD_BUTTONS]

    @Slot(str, result=int)
    def padCount(self, opt):
        return len([n for n in self.cfg.get(opt, "").split("+") if n]) if opt in COMBO_PADS else 0

    @Slot(str, str)
    def togglePad(self, opt, name):
        if opt not in COMBO_PADS:
            return
        on = [n for n in self.cfg[opt].split("+") if n]
        if name in on:
            on.remove(name)
        elif name in dict(PAD_BUTTONS) and len(on) < QUIT_MAX:
            on.append(name)
        self.setOpt(opt, "+".join(n for n, _ in PAD_BUTTONS if n in on))

    # ── atualizacao (launcher/updater.py) ───────────────────────────────────
    updChanged = Signal()
    _updDone = Signal(str, object)
    _updProgress = Signal(float)

    def _upd_cache_path(self) -> Path:
        return DATA / "config" / "update.json"

    def _upd_load_cache(self):
        """A ultima resposta do GitHub: mostra "nova versao" sem consultar de novo."""
        self._upd_checked = 0.0
        try:
            c = json.loads(self._upd_cache_path().read_text())
            self._upd_checked = float(c.get("checked") or 0)
            tag = str(c.get("tag") or "")
        except (OSError, ValueError, TypeError, AttributeError):
            return
        if updater.is_newer(tag, self.version):
            self._upd.update(state="available", tag=tag, page=updater.RELEASES_PAGE)

    def _upd_api(self) -> str:
        # So para os testes, e so para um servidor nesta mesma maquina.
        api = os.environ.get("DW4_UPDATE_API", "")
        return api if api.startswith("http://127.0.0.1:") else ""

    @Property(str, notify=updChanged)
    def updateState(self):
        return self._upd["state"]

    @Property(float, notify=updChanged)
    def updateProgress(self):
        return self._upd["progress"]

    @Property(bool, constant=True)
    def updateSelf(self):
        """Esta instalacao se atualiza sozinha (AppImage, .zip de Windows)."""
        return self._upd_kind != "manual"

    @Property(str, notify=updChanged)
    def updateText(self):
        t, u = STR[self._lang()], self._upd
        st = u["state"]
        if st == "error":
            return u["err"]
        if st == "available" and updater.parse_version(self.version) is None:
            return t["upd_dev"] % u["tag"]
        if st in ("current", "available", "downloading", "installing"):
            text = t["upd_" + st] % (self.version if st == "current" else u["tag"])
            return text + ("  %d%%" % round(u["progress"] * 100) if st == "downloading" else "")
        return t["upd_checking"] if st == "checking" else ""

    def _upd_set(self, **kw):
        self._upd.update(kw)
        self.updChanged.emit()

    def _upd_set_progress(self, v):
        if self._upd["state"] == "downloading":
            self._upd_set(progress=v)

    def _upd_error(self, e) -> str:
        t = STR[self._lang()]
        text = t.get("upd_err_" + e.code, t["upd_err_bad"])
        return text + scrub(e.detail) if e.code == "write" else text

    def autoCheckUpdate(self):
        """Ao abrir: so no pacote, com a opcao ligada e se a ultima consulta e antiga."""
        if (PACKAGED and self.cfg["check_updates"] and updater.parse_version(self.version)
                and time.time() - self._upd_checked >= UPDATE_EVERY):
            self.checkUpdate()

    @Slot()
    def checkUpdate(self):
        if self._upd_busy:
            return
        import threading
        self._upd_busy = True
        self._upd_set(state="checking", err="")
        kind, api = self._upd_kind, self._upd_api()

        def work():
            try:
                self._updDone.emit("checked", updater.check(kind, api))
            except updater.UpdateError as e:
                self._updDone.emit("error", e)
        threading.Thread(target=work, daemon=True).start()

    @Slot()
    def installUpdate(self):
        """Baixa, confere e instala; no fim o launcher fecha e o novo abre."""
        t = STR[self._lang()]
        if self._upd_busy:
            return
        if self.proc is not None:
            self.message.emit(t["upd_running"])
            return
        if self._upd_kind == "manual":
            self.openUpdatePage()
            return
        import threading
        self._upd_busy, self._upd_cancel = True, False
        self._upd_set(state="downloading", progress=0.0, err="")
        kind, api, version = self._upd_kind, self._upd_api(), self.version
        current = Path(os.path.realpath(os.environ.get("APPIMAGE") or "."))

        def work():
            try:
                rel = updater.check(kind, api)
                if not rel.asset_name:
                    raise updater.UpdateError("noasset")
                if not updater.is_newer(rel.tag, version):
                    self._updDone.emit("checked", rel)
                    return
                self._updDone.emit("tag", rel)
                cancelled = lambda: self._upd_cancel        # noqa: E731
                if kind == "appimage":
                    got = updater.download(rel, current.parent / (".dw4-update-" + rel.asset_name),
                                           self._updProgress.emit, cancelled)
                    self._updDone.emit("installing", rel)
                    self._updDone.emit("appimage", updater.apply_appimage(got, current, version, rel.tag))
                else:
                    work_dir = DATA / "update"
                    got = updater.download(rel, work_dir / rel.asset_name, self._updProgress.emit, cancelled)
                    self._updDone.emit("installing", rel)
                    root = updater.stage_windows(got, work_dir / "new")
                    got.unlink(missing_ok=True)
                    self._updDone.emit("windows", root)
            except updater.Cancelled:
                self._updDone.emit("cancelled", None)
            except updater.UpdateError as e:
                self._updDone.emit("error", e)
        threading.Thread(target=work, daemon=True).start()

    @Slot()
    def cancelUpdate(self):
        self._upd_cancel = True

    @Slot()
    def openUpdatePage(self):
        QDesktopServices.openUrl(QUrl(self._upd["page"] or updater.RELEASES_PAGE))

    def _upd_finished(self, what, arg):
        t = STR[self._lang()]
        if what == "tag":                   # o download vai comecar: ja se sabe a versao
            self._upd_set(tag=arg.tag, page=arg.page or updater.RELEASES_PAGE)
            return
        if what == "installing":
            self._upd_set(state="installing", progress=1.0)
            return
        self._upd_busy = False
        if what == "checked":
            self._upd_checked = time.time()
            try:
                self._upd_cache_path().parent.mkdir(parents=True, exist_ok=True)
                self._upd_cache_path().write_text(json.dumps({"checked": self._upd_checked, "tag": arg.tag}) + "\n")
            except OSError:
                pass
            newer = updater.is_newer(arg.tag, self.version) or updater.parse_version(self.version) is None
            self._upd_set(state="available" if newer else "current", tag=arg.tag,
                          page=arg.page or updater.RELEASES_PAGE)
        elif what == "error":
            self._upd_set(state="error", err=self._upd_error(arg))
        elif what == "cancelled":
            self._upd_set(state="available")
            self.message.emit(t["upd_cancelled"])
        elif what == "appimage":
            updater.relaunch_appimage(arg)
            QGuiApplication.quit()
        elif what == "windows":
            try:
                updater.start_windows_install(arg, APP.parent, os.getpid())
            except OSError as e:
                self._upd_set(state="error", err=t["upd_err_write"] + scrub(str(e)))
                return
            QGuiApplication.quit()

    # ── imagem de disco ─────────────────────────────────────────────────────
    isoChanged = Signal()
    _isoDone = Signal(str)

    @Property(bool, notify=isoChanged)
    def importing(self):
        return getattr(self, "_importing", False)

    @Property(float, notify=isoChanged)
    def importProgress(self):
        return getattr(self, "_progress", 0.0)

    def _set_progress(self, v):
        if v - getattr(self, "_progress", 0.0) >= 0.01 or v >= 1.0:
            self._progress = v
            self.isoChanged.emit()

    @Slot(QUrl)
    def importIso(self, url):
        """Copia os arquivos do jogo de uma imagem de disco para DATA/game_data."""
        if self.importing or self.proc is not None:
            return
        import threading
        import xiso
        src, dest = url.toLocalFile(), DATA / "game_data"
        self._importing, self._progress = True, 0.0
        self.isoChanged.emit()

        def work():
            t = STR[self._lang()]
            try:
                missing = xiso.extract(src, GAME_FILES, dest, self._set_progress)
                msg = t["iso_missing"] + ", ".join(missing) if missing else ""
            except xiso.XisoError:
                msg = t["iso_bad"]
            except OSError as e:
                msg = t["iso_io"] + (e.strerror or str(e))
            self._isoDone.emit(msg)
        if not getattr(self, "_iso_wired", False):
            self._isoDone.connect(self._iso_finished)
            self._iso_wired = True
        threading.Thread(target=work, daemon=True).start()

    def _iso_finished(self, err):
        self._importing = False
        self.isoChanged.emit()
        if err:
            self.message.emit(err)
            return
        self.cfg["game_dir"] = ""
        self._save()
        self.changed.emit()
        self.message.emit(STR[self._lang()]["iso_done"])

    @Slot()
    def resetGameDir(self):
        self.setOpt("game_dir", "")

    @Slot(str)
    def openFolder(self, which):
        d = {"saves": self.save_dir(), "logs": self.log_dir(), "game": self.game_dir()}.get(which)
        if d:
            d.mkdir(parents=True, exist_ok=True)
            QDesktopServices.openUrl(QUrl.fromLocalFile(str(d)))

    # ── jogar ───────────────────────────────────────────────────────────────
    def game_env(self) -> dict:
        env = dict(os.environ)
        c = self.cfg
        env["RECOMP_SAVE_DIR"] = str(self.save_dir())
        env["RECOMP_KEYBOARD"] = "1" if c["keyboard"] else "0"
        for k in ("RECOMP_DISPLAY_NAME", "RECOMP_DISPLAY", "RECOMP_DISPLAY_RANK", "RECOMP_DISPLAY_AT"):
            env.pop(k, None)
        env.update(self._display_env())
        env["RECOMP_USB_PADS"] = str(int(c["pads"]))        # portas de controle (toolkit, ohci.c)
        # So os comandos trocados: os outros ficam com o mapa do toolkit (patch 4.18).
        if c["keymap"]:
            env["RECOMP_KEYMAP"] = ";".join("%s=%s" % (ctl, TOOLKIT_KEY.get(k, k)) for ctl, k in c["keymap"].items())
        else:
            env.pop("RECOMP_KEYMAP", None)
        # Atalho que fecha o jogo (src/hooks/quit_hotkey.c). Um botao sozinho nao vale.
        env["DW4_QUIT_KEYS"] = c["quit_keys"] or "off"
        env["DW4_QUIT_PAD"] = c["quit_pad"] if c["quit_pad"].count("+") >= 1 else "off"
        # Mostrador de FPS / CPU / GPU (src/hooks/osd.c), com as mesmas regras de atalho.
        env["DW4_OSD"] = "1" if c["osd"] else "0"
        env["DW4_OSD_KEYS"] = c["osd_keys"] or "off"
        env["DW4_OSD_PAD"] = c["osd_pad"] if c["osd_pad"].count("+") >= 1 else "off"
        # Janela do tamanho da resolucao interna (toolkit: RECOMP_WINDOW_SIZE; ele
        # reduz se nao couber no monitor). 480p e 720p abrem no dobro.
        h = int(c["res"]) * (2 if int(c["res"]) < 1080 else 1)
        aw, ah = (int(x) for x in c["aspect"].split(":"))
        env["RECOMP_WINDOW_SIZE"] = "%dx%d" % (round(h * aw / ah), h)
        env.pop("RECOMP_WINDOW_SCALE", None)
        env["DW4_GL_NO_RELOAD"] = env.get("DW4_GL_NO_RELOAD", "1")
        if "DW4_SHOT_DIR" not in env:
            self.shot_dir().mkdir(parents=True, exist_ok=True)
            env["DW4_SHOT_DIR"] = str(self.shot_dir())
        # Uma linha a cada 5 s no log com fps, draws e onde vai o tempo: e o que
        # permite investigar uma queda de quadros depois, pelo relatorio.
        env.setdefault("DW4_VK_STATS", "1")
        env.setdefault("DW4_GL_STATS", "1")
        env["DW4_RENDERER"] = c["renderer"]     # opengl | vulkan (src/hooks/gl_host.c)
        env["DW4_RES"] = str(int(c["res"]))     # src/hooks/render_opts.h
        env["DW4_ASPECT"] = c["aspect"]
        env["DW4_FPS_LIMIT"] = str(int(c["fps"]))   # src/hooks/fps_limit.c
        # O rasterizador em CPU saiu do launcher; segue existindo para quem desenvolve
        # (DW4_SW_RENDER=1 ./run.sh). Uma variavel herdada do ambiente nao pode liga-lo aqui.
        for k, on in (("RECOMP_FULLSCREEN", c["fullscreen"]), ("DW4_SW_RENDER", False)):
            if on:
                env[k] = "1"
            else:
                env.pop(k, None)
        if not c["vsync"]:
            env["RECOMP_PRESENT_VSYNC"] = "0"
        if c["parallel"]:
            env["RECOMP_KICK_AHEAD"] = "1"      # toolkit, patch 4.13 (PORT_NOTES)
        else:
            env.pop("RECOMP_KICK_AHEAD", None)
        # O launcher congelado roda com o LD_LIBRARY_PATH do PyInstaller (Qt etc.);
        # o jogo nao pode herdar isso: volta ao valor original do usuario.
        if FROZEN:
            orig = env.pop("LD_LIBRARY_PATH_ORIG", None)
            if orig is not None:
                env["LD_LIBRARY_PATH"] = orig
            else:
                env.pop("LD_LIBRARY_PATH", None)
        if WINE:
            env.setdefault("WINEPREFIX", str(WINE_PREFIX))
            env.setdefault("WINEDEBUG", "-all")
            env.setdefault("WINEDLLOVERRIDES", "mscoree,mshtml=")
        lib = APP / "lib"                       # pacotes de release
        if lib.is_dir() and (FROZEN or not self._system_has_crypto(lib)):
            # No pacote completo as bibliotecas embutidas (SDL2, libepoxy, libcrypto)
            # valem sempre: e com elas que o pacote foi testado.
            env["LD_LIBRARY_PATH"] = str(lib) + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        return env

    @staticmethod
    def _system_has_crypto(lib: Path) -> bool:
        names = [p.name for p in lib.glob("libcrypto.so.*")]
        if not names:
            return True
        try:
            out = subprocess.run(["ldconfig", "-p"], capture_output=True, text=True, timeout=4).stdout
        except (OSError, subprocess.SubprocessError):
            return False
        return any(n in out for n in names)

    def _new_log(self) -> Path:
        d = self.log_dir()
        d.mkdir(parents=True, exist_ok=True)
        old = sorted(d.glob("run-*.log"))
        for p in old[:max(0, len(old) - (KEEP_LOGS - 1))]:
            p.unlink(missing_ok=True)
        return d / time.strftime("run-%Y%m%d-%H%M%S.log")

    def _config_header(self) -> str:
        c = self.cfg
        return ("Config: launcher %s | renderer=%s res=%s aspect=%s fps=%s parallel=%s fullscreen=%s vsync=%s "
                "keyboard=%s pads=%s keymap=%s quit=%s/%s osd=%s,%s/%s | GPU: %s\n"
                % (self.version, c["renderer"], c["res"], c["aspect"], c["fps"], c["parallel"], c["fullscreen"],
                   c["vsync"], c["keyboard"], c["pads"], len(c["keymap"]), c["quit_keys"] or "off", c["quit_pad"] or "off",
                   c["osd"], c["osd_keys"] or "off", c["osd_pad"] or "off", self._gpu))

    @Slot()
    def play(self):
        if self.proc is not None or not self.canPlay:
            return
        log = self._new_log()
        log.write_text(self._config_header())
        proc = QProcess(self)
        qenv = QProcessEnvironment()
        for k, v in self.game_env().items():
            qenv.insert(k, v)
        proc.setProcessEnvironment(qenv)
        DATA.mkdir(parents=True, exist_ok=True)
        proc.setWorkingDirectory(str(DATA))
        proc.setProcessChannelMode(QProcess.MergedChannels)
        proc.setStandardOutputFile(str(log), QProcess.Append)
        proc.finished.connect(self._finished)
        proc.errorOccurred.connect(self._proc_error)
        self.proc, self.last_log = proc, log
        self.runningChanged.emit()
        cmd = self.command()
        proc.start(cmd[0], cmd[1:])

    def _proc_error(self, err):
        if err == QProcess.FailedToStart:
            self._finished(-1, QProcess.CrashExit)

    def _finished(self, code, status):
        if self.proc is None:
            return
        self.proc.deleteLater()
        self.proc = None
        t = STR[self._lang()]
        crashed = status == QProcess.CrashExit or code != 0
        if not crashed and self.last_log:
            try:
                with open(self.last_log, "rb") as f:
                    f.seek(max(0, self.last_log.stat().st_size - 200000))
                    crashed = b"[CRASH]" in f.read()
            except OSError:
                pass
        self.message.emit(t["ended_crash"] + scrub(str(self.last_log)) if crashed else t["ended_ok"])
        self.runningChanged.emit()
        self.changed.emit()

    # ── saves ───────────────────────────────────────────────────────────────
    def _zip_saves(self, dest: Path) -> bool:
        src = self.save_dir()
        files = [f for f in src.rglob("*") if f.is_file()] if src.is_dir() else []
        if not files:
            return False
        dest.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("dw4-recomp-saves.txt", "Digimon World 4 - Recomp save backup\n")
            for f in files:
                z.write(f, Path("save_data") / f.relative_to(src))
        return True

    def _backup_path(self) -> Path:
        return DATA / "backups" / time.strftime("saves-%Y%m%d-%H%M%S.zip")

    @Slot()
    def backupSaves(self):
        t = STR[self._lang()]
        dest = self._backup_path()
        self.message.emit(t["backup_done"] + scrub(str(dest)) if self._zip_saves(dest) else t["no_saves"])

    @Slot(QUrl)
    def exportSaves(self, url):
        t = STR[self._lang()]
        dest = Path(url.toLocalFile())
        if dest.suffix.lower() != ".zip":
            dest = dest.with_name(dest.name + ".zip")
        self.message.emit(t["export_done"] + scrub(str(dest)) if self._zip_saves(dest) else t["no_saves"])

    @Slot(QUrl)
    def importSaves(self, url):
        t = STR[self._lang()]
        if self.proc is not None:
            self.message.emit(t["not_while_running"])
            return
        try:
            with zipfile.ZipFile(url.toLocalFile()) as z:
                names = [n for n in z.namelist() if n.startswith("save_data/") and not n.endswith("/")]
                # so caminhos relativos dentro de save_data/: nada de "..", absolutos ou links
                ok = "dw4-recomp-saves.txt" in z.namelist() and names and all(
                    ".." not in Path(n).parts and not Path(n).is_absolute()
                    and (z.getinfo(n).external_attr >> 16) & 0o170000 in (0, 0o100000) for n in names)
                if not ok:
                    self.message.emit(t["import_bad"])
                    return
                backup = self._backup_path()
                had = self._zip_saves(backup)
                dst = self.save_dir()
                if dst.is_dir():
                    shutil.rmtree(dst)
                for n in names:
                    out = DATA / n
                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(z.read(n))
        except (OSError, zipfile.BadZipFile):
            self.message.emit(t["import_bad"])
            return
        self.message.emit(t["import_done"] + (scrub(str(backup)) if had else "—"))
        self.changed.emit()

    # ── suporte ─────────────────────────────────────────────────────────────
    @staticmethod
    def _perf_lines(log: Path) -> list:
        """Do log inteiro (o fim sozinho pode nao ter): como o jogo apresenta e a faixa de fps."""
        mode, fps, worst = "", [], ""
        try:
            for ln in open(log, encoding="utf-8", errors="replace"):
                if "] apresentacao:" in ln or "renderizador ativo:" in ln:
                    mode += " | " + ln.strip()
                elif " fps | por frame" in ln:
                    try:
                        v = float(ln.split("]", 1)[1].split("fps", 1)[0])
                    except (IndexError, ValueError):
                        continue
                    if not fps or v < min(fps):
                        worst = ln.strip()
                    fps.append(v)
        except OSError:
            return []
        out = ["presentation:" + (mode or " ?")]
        if fps:
            out.append("fps (5 s samples): min %.1f, median %.1f, max %.1f, n=%d"
                       % (min(fps), sorted(fps)[len(fps) // 2], max(fps), len(fps)))
            out.append("slowest sample: " + worst)
        return out

    @staticmethod
    def _cpu_name() -> str:
        if WIN:
            return (Backend._win_reg(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString")
                    or platform.processor() or "?")
        try:
            for ln in open("/proc/cpuinfo", encoding="utf-8", errors="replace"):
                if ln.startswith("model name"):
                    return ln.split(":", 1)[1].strip()
        except OSError:
            pass
        return platform.processor() or platform.machine() or "?"

    @Slot()
    def makeReport(self):
        t = STR[self._lang()]
        logs = sorted(self.log_dir().glob("run-*.log"))
        if not logs:
            self.message.emit(t["no_log"])
            return
        last = logs[-1]
        with open(last, "rb") as f:
            f.seek(max(0, last.stat().st_size - 60000))
            tail = f.read().decode("utf-8", "replace").splitlines()[-300:]
        if WIN:
            system = "%s | cpu: %s, %s threads" % (platform.platform(), self._cpu_name(), os.cpu_count() or "?")
        else:
            try:
                osr = platform.freedesktop_os_release().get("PRETTY_NAME", "?")
            except (OSError, AttributeError):
                osr = "?"
            system = ("%s | kernel %s | glibc %s | cpu: %s, %s threads"
                      % (osr, platform.release(), platform.libc_ver()[1], self._cpu_name(), os.cpu_count() or "?"))
        body = ["Digimon World 4 - Recomp: report", "version: " + self.version,
                "system: " + system,
                "gpu: " + self._gpu, "options: " + json.dumps({k: v for k, v in self.cfg.items() if k != "game_dir"}),
                "game files present: " + ", ".join(n for n in GAME_FILES if n not in self.missing_files()),
                "log: " + last.name] + self._perf_lines(last) + ["-" * 70] + tail
        dest = self.log_dir() / time.strftime("report-%Y%m%d-%H%M%S.txt")
        dest.write_text(scrub("\n".join(body)) + "\n")
        self.message.emit(t["report_done"] + scrub(str(dest)))


def play_direct() -> int:
    b = Backend()
    if not b.canPlay:
        print(b.statusText, file=sys.stderr)
        return 1
    log = b._new_log()
    with open(log, "w") as f:
        f.write(b._config_header())
        f.flush()
        return subprocess.call(b.command(), env=b.game_env(), cwd=DATA,
                               stdout=f, stderr=subprocess.STDOUT)


def main() -> int:
    args = sys.argv[1:]
    if "--play" in args:
        return play_direct()
    offscreen = "--selftest" in args or "--screenshot" in args
    if os.environ.get("DW4_QPA"):
        # Teste do pacote de Windows no Wine: ele nao repassa variaveis QT_* ao
        # programa, e a plataforma "offscreen" do Qt no Windows nao tem fontes
        # (o texto sai em quadradinhos). DW4_QPA=windows captura com a real.
        os.environ["QT_QPA_PLATFORM"] = os.environ["DW4_QPA"]
    if offscreen:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        os.environ.setdefault("QT_QUICK_BACKEND", "software")
    os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "Basic")
    if WIN:
        # Sem isto a barra de tarefas agrupa a janela com o "pythonw.exe" que a abriu
        # e mostra o icone dele, nao o do projeto.
        try:
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("dw4recomp.launcher")
        except (OSError, AttributeError):
            pass
    app = QGuiApplication(sys.argv[:1])
    app.setApplicationName("Digimon World 4 - Recomp")
    for icon in (HERE / "dw4-recomp.png", ROOT / "packaging" / "dw4-recomp.png"):
        if icon.is_file():              # icone da janela (no Windows vinha o generico)
            app.setWindowIcon(QIcon(str(icon)))
            break
    app.setDesktopFileName("dw4-recomp-launcher")
    backend = Backend()
    if "--screenshot" in args:          # so para a captura; nada disto e gravado
        if "--defaults" in args:        # opcoes de fabrica, sem a pasta de jogo do usuario
            backend.cfg = dict(DEFAULTS)
        if "--lang" in args:
            backend.cfg["lang"] = args[args.index("--lang") + 1]
    engine = QQmlApplicationEngine()
    engine.rootContext().setContextProperty("app", backend)
    engine.load(QUrl.fromLocalFile(str(HERE / "qml" / "Main.qml")))
    if not engine.rootObjects():
        print("selftest FAILED: QML did not load", file=sys.stderr)
        return 1
    win = engine.rootObjects()[0]
    if "--selftest" in args:
        assert set(STR["pt"]) == set(STR["en"]), "tabelas de texto diferentes entre idiomas"
        import xiso  # noqa: F401  (confere que o leitor de imagem veio no pacote)
        import ssl  # noqa: F401  (a atualizacao precisa de HTTPS)
        assert all(key_valid(k) for k in KEY_NAMES.values())
        assert all(key_valid(k) for k in KEYMAP_DEFAULT.values())
        exe = backend.exe()
        print("app:", "frozen" if FROZEN else "packaged" if PACKAGED else "source", "| wine:" if WINE else "|", "game exe:", "ok" if exe else "MISSING",
              "| data:", scrub(str(DATA)))
        print("selftest ok")
        return 0
    if "--screenshot" in args:
        i = args.index("--screenshot")
        win.setProperty("page", int(args[i + 1]))
        if "--size" in args:            # a tela "offscreen" e pequena e limitaria a janela
            w, _, h = args[args.index("--size") + 1].partition("x")
            win.setProperty("width", int(w))
            win.setProperty("height", int(h))
        if "--open-combo" in args:          # captura com um select aberto (teste visual)
            QTimer.singleShot(150, lambda: win.setProperty("debugOpenCombo", True))

        def grab():
            try:
                win.grabWindow().save(args[i + 2])
            finally:
                app.quit()
        QTimer.singleShot(400, grab)
    else:
        if PACKAGED:                    # restos de uma atualizacao anterior (pasta de preparo)
            import threading
            threading.Thread(target=updater.cleanup, args=(DATA / "update",), daemon=True).start()
        QTimer.singleShot(1200, backend.autoCheckUpdate)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
