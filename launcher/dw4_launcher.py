#!/usr/bin/env python3
"""Launcher do Digimon World 4 - Recomp (Qt Quick, PySide6).

    launcher.sh                 abre o launcher
    launcher.sh --play          inicia o jogo direto, com as opcoes salvas
    launcher.sh --selftest      carrega a interface fora da tela e sai ("selftest ok")
    launcher.sh --screenshot <pagina> <arquivo.png>   (documentacao/testes)

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

from PySide6.QtCore import (Property, QObject, QProcess, QProcessEnvironment, QTimer, QUrl,
                            Signal, Slot)
from PySide6.QtGui import QDesktopServices, QGuiApplication
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickWindow  # noqa: F401  (registra o tipo da janela)

HERE = Path(__file__).resolve().parent          # no pacote congelado: pasta de dados do PyInstaller
ROOT = HERE.parent
FROZEN = bool(getattr(sys, "frozen", False))

# APP: onde estao o executavel do jogo e lib/ (so leitura dentro de um AppImage).
# DATA: tudo o que se grava (opcoes, logs, saves, backups, game_data/).
#   - no repositorio e no pacote .tar.gz antigo: a propria pasta do projeto;
#   - no AppImage / pacote congelado: ~/.local/share/dw4-recomp (XDG_DATA_HOME).
APP = Path(os.environ.get("DW4_APP_DIR") or (Path(sys.executable).resolve().parent.parent / "game" if FROZEN else ROOT))
DATA = Path(os.environ.get("DW4_DATA_DIR") or (
    Path(os.environ.get("XDG_DATA_HOME") or Path.home() / ".local" / "share") / "dw4-recomp" if FROZEN else ROOT))
GAME_FILES = ["default.xbe", "area.afs", "ndmw.afs", "ndmw.igb"]
TITLE_ID = "42410002"
KEEP_LOGS = 10

DEFAULTS = {
    "game_dir": "",          # vazio = <raiz>/game_data
    "lang": "auto",          # auto | pt | en
    "fullscreen": False,
    "scale": 2,              # janela = 640x480 x escala
    "vsync": True,
    "renderer": "gpu",       # gpu | cpu
    "keyboard": True,
}

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
        "window": "Janela", "fullscreen": "Tela cheia", "fullscreen_d": "Tela cheia sem borda, na resolução da área de trabalho.",
        "scale": "Tamanho da janela", "scale_d": "Múltiplo de 640×480. A imagem do jogo continua em 640×480.",
        "vsync": "Sincronização vertical", "vsync_d": "Apresenta os quadros no ritmo do monitor. Desligue só para testes.",
        "renderer": "Renderizador", "r_gpu": "GPU (OpenGL 3.3) — recomendado", "r_cpu": "CPU (referência, muito lento)",
        "renderer_d": "O de CPU faz poucos quadros por segundo; serve para comparar quando a imagem parece errada.",
        "keyboard": "Teclado como controle 1", "keyboard_d": "Um controle conectado continua funcionando junto.",
        "pad_note": "Qualquer controle reconhecido como Xbox/XInput funciona sem configuração.",
        "key": "Tecla", "xbox": "Controle do Xbox",
        "save_folder": "Pasta dos saves", "open_folder": "Abrir pasta", "backup": "Fazer backup",
        "export": "Exportar .zip…", "import": "Importar .zip…", "no_saves": "Nenhum save ainda.",
        "save_list": "Saves encontrados", "backup_done": "Backup gravado em: ",
        "export_done": "Saves exportados para: ", "import_done": "Saves importados. O backup dos anteriores está em: ",
        "import_bad": "Este .zip não parece um backup de saves deste launcher.",
        "not_while_running": "Feche o jogo antes de mexer nos saves.",
        "known": "Problemas conhecidos",
        "known_1": "Pular o vídeo de abertura com START às vezes fecha o jogo. Abra de novo; deixar o vídeo tocar evita.",
        "known_2": "Reflexos e brilhos gerados automaticamente não estão implementados; alguns objetos podem parecer chapados.",
        "known_3": "Testado em uma única máquina (CachyOS, NVIDIA). Outros sistemas ainda não foram testados.",
        "logs": "Logs", "open_logs": "Abrir pasta de logs", "report": "Gerar relatório",
        "report_d": "Junta versão, sistema, placa de vídeo, opções e o fim do último log em um arquivo de texto, "
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
        "window": "Window", "fullscreen": "Fullscreen", "fullscreen_d": "Borderless fullscreen at the desktop resolution.",
        "scale": "Window size", "scale_d": "A multiple of 640×480. The game still renders at 640×480.",
        "vsync": "Vertical sync", "vsync_d": "Presents frames at the display's pace. Turn off for testing only.",
        "renderer": "Renderer", "r_gpu": "GPU (OpenGL 3.3) — recommended", "r_cpu": "CPU (reference, very slow)",
        "renderer_d": "The CPU one runs at a few frames per second; use it to compare when the picture looks wrong.",
        "keyboard": "Keyboard as controller 1", "keyboard_d": "A connected controller keeps working alongside it.",
        "pad_note": "Any controller the system reports as Xbox/XInput works with no setup.",
        "key": "Key", "xbox": "Xbox control",
        "save_folder": "Save folder", "open_folder": "Open folder", "backup": "Back up",
        "export": "Export .zip…", "import": "Import .zip…", "no_saves": "No saves yet.",
        "save_list": "Saves found", "backup_done": "Backup written to: ",
        "export_done": "Saves exported to: ", "import_done": "Saves imported. The previous ones were backed up to: ",
        "import_bad": "This .zip does not look like a save backup from this launcher.",
        "not_while_running": "Close the game before touching the saves.",
        "known": "Known issues",
        "known_1": "Skipping the intro movie with START sometimes makes the game quit. Start again; letting it play avoids it.",
        "known_2": "Automatically generated reflections and shine are not implemented; some objects may look flat.",
        "known_3": "Tested on one machine (CachyOS, NVIDIA). Other systems are untested.",
        "logs": "Logs", "open_logs": "Open log folder", "report": "Create report",
        "report_d": "Collects version, system, graphics card, options and the end of the last log into a text file, "
                    "without your home folder's name. No game file is included.",
        "report_done": "Report written to: ", "no_log": "No log yet: play once through the launcher.",
        "language": "Launcher language", "l_auto": "Automatic", "freeze_tip":
            "If the game freezes, wait about 10 seconds before closing: the log records where it stopped.",
    },
}

KEYMAP = [("Enter", "START"), ("Backspace", "BACK"), ("Z / X / C / V", "A / B / X / Y"),
          ("↑ ↓ ← →", "D-pad"), ("W A S D", "L stick"), ("I J K L", "R stick"),
          ("Q / E", "White / Black"), ("1 / 3", "L / R trigger"), ("Shift / Ctrl", "L3 / R3")]


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
        self.proc = None
        self.log_file = None
        self.last_log = None
        self._gpu = self._detect_gpu()

    # ── caminhos ────────────────────────────────────────────────────────────
    def exe(self):
        for p in (APP / "dw4_recomp", APP / "build" / "dw4_recomp"):
            if p.is_file() and os.access(p, os.X_OK):
                return p
        return None

    def game_dir(self) -> Path:
        return Path(self.cfg["game_dir"]) if self.cfg["game_dir"] else DATA / "game_data"

    def save_dir(self) -> Path:
        return DATA / "save_data"

    def log_dir(self) -> Path:
        return DATA / "logs"

    def missing_files(self):
        d = self.game_dir()
        return [n for n in GAME_FILES if not find_ci(d, n)]

    # ── deteccao ────────────────────────────────────────────────────────────
    @staticmethod
    def _detect_gpu() -> str:
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

    def _save(self):
        self.cfg_path.parent.mkdir(parents=True, exist_ok=True)
        self.cfg_path.write_text(json.dumps(self.cfg, indent=2) + "\n")

    def _lang(self) -> str:
        if self.cfg["lang"] in STR:
            return self.cfg["lang"]
        loc = (os.environ.get("LC_ALL") or os.environ.get("LC_MESSAGES") or os.environ.get("LANG")
               or (locale.getlocale()[0] or ""))
        return "pt" if loc.lower().startswith("pt") else "en"

    # ── propriedades para o QML ─────────────────────────────────────────────
    @Property("QVariantMap", notify=langChanged)
    def t(self):
        return STR[self._lang()]

    @Property("QVariantList", constant=True)
    def keymap(self):
        return [{"key": k, "xbox": x} for k, x in KEYMAP]

    @Property(str, constant=True)
    def gpu(self):
        return self._gpu

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
        self._save()
        if key == "lang":
            self.langChanged.emit()
        self.changed.emit()

    @Slot(QUrl)
    def setGameDir(self, url):
        self.setOpt("game_dir", url.toLocalFile())

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
        env["RECOMP_WINDOW_SCALE"] = str(int(c["scale"]))
        env["DW4_GL_NO_RELOAD"] = env.get("DW4_GL_NO_RELOAD", "1")
        for k, on in (("RECOMP_FULLSCREEN", c["fullscreen"]), ("DW4_SW_RENDER", c["renderer"] == "cpu")):
            if on:
                env[k] = "1"
            else:
                env.pop(k, None)
        if not c["vsync"]:
            env["RECOMP_PRESENT_VSYNC"] = "0"
        # O launcher congelado roda com o LD_LIBRARY_PATH do PyInstaller (Qt etc.);
        # o jogo nao pode herdar isso: volta ao valor original do usuario.
        if FROZEN:
            orig = env.pop("LD_LIBRARY_PATH_ORIG", None)
            if orig is not None:
                env["LD_LIBRARY_PATH"] = orig
            else:
                env.pop("LD_LIBRARY_PATH", None)
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
        return ("Config: launcher %s | renderer=%s fullscreen=%s scale=%s vsync=%s keyboard=%s | GPU: %s\n"
                % (self.version, c["renderer"], c["fullscreen"], c["scale"], c["vsync"], c["keyboard"], self._gpu))

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
        proc.start(str(self.exe()), [str(self.game_dir())])

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
        osr = platform.freedesktop_os_release().get("PRETTY_NAME", "?") if hasattr(platform, "freedesktop_os_release") else "?"
        body = ["Digimon World 4 - Recomp: report", "version: " + self.version,
                "system: %s | kernel %s | glibc %s" % (osr, platform.release(), platform.libc_ver()[1]),
                "gpu: " + self._gpu, "options: " + json.dumps({k: v for k, v in self.cfg.items() if k != "game_dir"}),
                "game files present: " + ", ".join(n for n in GAME_FILES if n not in self.missing_files()),
                "log: " + last.name, "-" * 70] + tail
        dest = self.log_dir() / time.strftime("report-%Y%m%d-%H%M%S.txt")
        dest.write_text(scrub("\n".join(body)) + "\n")
        self.message.emit(t["report_done"] + scrub(str(dest)))


sys.path.insert(0, str(HERE))


def play_direct() -> int:
    b = Backend()
    if not b.canPlay:
        print(b.statusText, file=sys.stderr)
        return 1
    log = b._new_log()
    with open(log, "w") as f:
        f.write(b._config_header())
        f.flush()
        return subprocess.call([str(b.exe()), str(b.game_dir())], env=b.game_env(), cwd=DATA,
                               stdout=f, stderr=subprocess.STDOUT)


def main() -> int:
    args = sys.argv[1:]
    if "--play" in args:
        return play_direct()
    offscreen = "--selftest" in args or "--screenshot" in args
    if offscreen:
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        os.environ.setdefault("QT_QUICK_BACKEND", "software")
    os.environ.setdefault("QT_QUICK_CONTROLS_STYLE", "Basic")
    app = QGuiApplication(sys.argv[:1])
    app.setApplicationName("Digimon World 4 - Recomp")
    app.setDesktopFileName("dw4-recomp-launcher")
    backend = Backend()
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
        exe = backend.exe()
        print("app:", "frozen" if FROZEN else "source", "| game exe:", "ok" if exe else "MISSING",
              "| data:", scrub(str(DATA)))
        print("selftest ok")
        return 0
    if "--screenshot" in args:
        i = args.index("--screenshot")
        win.setProperty("page", int(args[i + 1]))
        if "--open-combo" in args:          # captura com um select aberto (teste visual)
            QTimer.singleShot(150, lambda: win.setProperty("debugOpenCombo", True))

        def grab():
            try:
                win.grabWindow().save(args[i + 2])
            finally:
                app.quit()
        QTimer.singleShot(400, grab)
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
