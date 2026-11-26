"""Atualizacao do Digimon World 4 - Recomp pelos releases do GitHub.

Sem Qt: so a biblioteca padrao, para poder ser testado sozinho
(scripts/test_updater.py) e rodar no ajudante de instalacao do Windows antes
de qualquer janela.

O que faz:
  check()            pergunta ao GitHub qual e o release mais recente
  download()         baixa o pacote deste sistema e confere tamanho e SHA-256
  apply_appimage()   Linux/AppImage: poe o arquivo novo no lugar do atual
  stage_windows()    Windows: extrai o .zip novo numa pasta de preparo
  install_windows()  Windows: copia a pasta preparada sobre a instalacao
                     (rodado pelo launcher NOVO, a partir da pasta de preparo,
                     depois que o antigo fechou: --install-update)

Quem pode se atualizar sozinho: o AppImage (numa pasta gravavel) e o pacote
.zip de Windows. O .tar.gz de Linux e o codigo-fonte so sao avisados e abrem a
pagina do release.

A ideia (baixar o asset do sistema, conferir o hash, trocar o AppImage por
renomeacao, no Windows instalar a partir de uma copia temporaria do launcher
novo) e a do launcher do SpartanPort (FelipePrado0/SpartanPort, GPL-2); nenhum
codigo de la foi copiado. Diferencas: aqui o hash e obrigatorio nos dois
sistemas, o download so e aceito do repositorio do projeto, tem limite de
tempo por leitura e pode ser cancelado.
"""
import hashlib
import json
import os
import re
import shutil
import ssl
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

REPO = "lorransouzaaguiar/Digimon-World-4-Recomp"
API_LATEST = "https://api.github.com/repos/%s/releases/latest" % REPO
RELEASES_PAGE = "https://github.com/%s/releases/latest" % REPO
DOWNLOAD_PREFIX = "https://github.com/%s/releases/download/" % REPO
USER_AGENT = "dw4-recomp-launcher"
WIN_STUB = "DigimonWorld4-Recomp.exe"
ASSET_SUFFIX = {"appimage": "-x86_64.AppImage", "windows": "-windows-x86_64.zip"}
MAX_JSON = 2 * 1024 * 1024
MAX_ASSET = 1024 * 1024 * 1024           # nenhum pacote passa perto de 1 GB

WIN = os.name == "nt"


def _long(path) -> str:
    """Caminho que o Windows aceita com mais de 260 caracteres (prefixo \\\\?\\).

    A atualizacao da v0.5.0 para a v0.5.1 parou num arquivo do Qt cujo caminho,
    dentro da pasta de preparo em %APPDATA%, passava do limite ("No such file or
    directory" ao criar o arquivo). Fora do Windows devolve o caminho como veio."""
    p = os.path.abspath(str(path))
    if not WIN or p.startswith("\\\\?\\"):
        return p
    return "\\\\?\\UNC\\" + p[2:] if p.startswith("\\\\") else "\\\\?\\" + p


class UpdateError(Exception):
    """`code` escolhe o texto mostrado ao usuario (STR["upd_err_<code>"])."""

    def __init__(self, code: str, detail: str = ""):
        super().__init__("%s: %s" % (code, detail) if detail else code)
        self.code = code
        self.detail = detail


class Cancelled(Exception):
    pass


class Release:
    def __init__(self, tag, page, asset_name="", asset_url="", size=0, sha256=""):
        self.tag = tag                  # "v0.5.0"
        self.page = page                # pagina do release (notas)
        self.asset_name = asset_name    # "" = este release nao tem pacote para este tipo de instalacao
        self.asset_url = asset_url
        self.size = size
        self.sha256 = sha256


def parse_version(text):
    """"v0.4.1" -> (0, 4, 1); None para qualquer outra coisa ("dev", "v1.0-rc1")."""
    m = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", (text or "").strip())
    return tuple(int(x) for x in m.groups()) if m else None


def is_newer(remote: str, local: str) -> bool:
    r, l = parse_version(remote), parse_version(local)
    return r is not None and l is not None and r > l


def _writable(folder: Path) -> bool:
    probe = folder / (".dw4-write-test-%d" % os.getpid())
    try:
        probe.write_bytes(b"")
        probe.unlink()
        return True
    except OSError:
        return False


def install_kind(frozen: bool, packaged: bool, app_dir: Path) -> str:
    """"appimage" | "windows" (atualizam sozinhos) | "manual" (so avisa)."""
    if WIN:
        root = Path(app_dir).parent
        if packaged and (root / WIN_STUB).is_file() and _writable(root):
            return "windows"
        return "manual"
    img = os.environ.get("APPIMAGE")
    if frozen and img:
        p = Path(os.path.realpath(img))
        if p.is_file() and _writable(p.parent):
            return "appimage"
    return "manual"


def _ssl_context():
    ctx = ssl.create_default_context()
    if not WIN:
        # O Python congelado no pacote de Linux pode ter sido compilado com
        # outro caminho de certificados: soma o pacote de CAs do sistema.
        for ca in ("/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
                   "/etc/ssl/ca-bundle.pem", "/etc/ssl/cert.pem",
                   "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"):
            if os.path.isfile(ca):
                try:
                    ctx.load_verify_locations(cafile=ca)
                    break
                except (OSError, ssl.SSLError):
                    continue
    return ctx


def _open(url: str, timeout: float, accept: str = ""):
    headers = {"User-Agent": USER_AGENT}
    if accept:
        headers["Accept"] = accept
    req = urllib.request.Request(url, headers=headers)
    ctx = _ssl_context() if url.lower().startswith("https:") else None
    try:
        return urllib.request.urlopen(req, timeout=timeout, context=ctx)
    except urllib.error.HTTPError as e:
        # 403/429 sem autenticacao = limite de consultas por hora da API do GitHub.
        raise UpdateError("rate" if e.code in (403, 429) else "http", "HTTP %d" % e.code)
    except (urllib.error.URLError, OSError, ValueError) as e:
        raise UpdateError("net", str(getattr(e, "reason", e)))


def _allowed(url: str, prefix: str) -> bool:
    return isinstance(url, str) and url.startswith(prefix)


def check(kind: str, api: str = "", timeout: float = 15.0) -> Release:
    """O release mais recente (nao rascunho, nao pre-release) e o pacote dele
    para `kind`. `api` troca o endereco consultado (so os testes usam; ai o
    download tambem e aceito do mesmo servidor)."""
    url = api or API_LATEST
    prefix = url.rsplit("/", 1)[0] + "/" if api else DOWNLOAD_PREFIX
    with _open(url, timeout, "application/vnd.github+json") as r:
        raw = r.read(MAX_JSON + 1)
    try:
        if len(raw) > MAX_JSON:
            raise ValueError("resposta grande demais")
        d = json.loads(raw.decode("utf-8"))
        tag = d["tag_name"]
        if parse_version(tag) is None or d.get("draft") or d.get("prerelease"):
            raise ValueError("release sem versao vX.Y.Z: %r" % (tag,))
        assets = d.get("assets") or []
        page = d.get("html_url") or ""
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        raise UpdateError("bad", str(e))
    if not api and not _allowed(page, "https://github.com/%s/" % REPO):
        page = RELEASES_PAGE
    rel = Release(tag, page)
    suffix = ASSET_SUFFIX.get(kind)
    if not suffix:
        return rel
    sums_url = ""
    for a in assets:
        name, aurl = str(a.get("name") or ""), a.get("browser_download_url") or ""
        if not _allowed(aurl, prefix) or "/" in name or "\\" in name:
            continue
        if name == "SHA256SUMS-%s.txt" % tag:
            sums_url = aurl
        elif name.endswith(suffix) and tag in name and not rel.asset_name:
            digest = str(a.get("digest") or "")
            rel.asset_name, rel.asset_url = name, aurl
            rel.size = int(a.get("size") or 0)
            rel.sha256 = digest[7:].lower() if digest.startswith("sha256:") else ""
    if rel.asset_name and not rel.sha256 and sums_url:
        # Releases antigos da API nao trazem "digest": o arquivo de checksums do release.
        with _open(sums_url, timeout) as r:
            for ln in r.read(65536).decode("utf-8", "replace").splitlines():
                parts = ln.split()
                if len(parts) == 2 and parts[1].lstrip("*") == rel.asset_name:
                    rel.sha256 = parts[0].lower()
    if rel.asset_name and not re.fullmatch(r"[0-9a-f]{64}", rel.sha256):
        raise UpdateError("bad", "release sem SHA-256 para " + rel.asset_name)
    return rel


def download(rel: Release, dest: Path, progress=None, cancelled=None, timeout: float = 30.0) -> Path:
    """Baixa o pacote para `dest` (por um ".part" ao lado) e so o entrega se o
    tamanho e o SHA-256 baterem com os do release. `timeout` vale por leitura:
    conexao parada no meio nao trava para sempre."""
    dest = Path(dest)
    part = dest.with_name(dest.name + ".part")
    h, got = hashlib.sha256(), 0
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        with _open(rel.asset_url, timeout) as r, open(part, "wb") as f:
            total = rel.size or int(r.headers.get("Content-Length") or 0)
            if total > MAX_ASSET:
                raise UpdateError("bad", "pacote grande demais")
            while True:
                if cancelled and cancelled():
                    raise Cancelled()
                try:
                    chunk = r.read(1 << 18)
                except OSError as e:            # inclui o timeout de leitura
                    raise UpdateError("net", str(e))
                if not chunk:
                    break
                got += len(chunk)
                if got > MAX_ASSET or (rel.size and got > rel.size):
                    raise UpdateError("hash", "veio mais do que o release anuncia")
                h.update(chunk)
                f.write(chunk)
                if progress and total:
                    progress(min(1.0, got / total))
        if (rel.size and got != rel.size) or h.hexdigest() != rel.sha256:
            raise UpdateError("hash", "%d bytes, sha256 %s" % (got, h.hexdigest()))
        os.replace(part, dest)
        return dest
    except UpdateError:
        part.unlink(missing_ok=True)
        raise
    except Cancelled:
        part.unlink(missing_ok=True)
        raise
    except OSError as e:
        part.unlink(missing_ok=True)
        raise UpdateError("write", e.strerror or str(e))


# ── Linux: AppImage ──────────────────────────────────────────────────────────
def appimage_target(current: Path, old_tag: str, new_tag: str) -> Path:
    """Onde fica o AppImage novo: o nome do atual com a versao trocada; se o
    usuario renomeou o arquivo (sem a versao no nome), o mesmo nome."""
    current = Path(current)
    if old_tag and old_tag != new_tag and old_tag in current.name:
        return current.with_name(current.name.replace(old_tag, new_tag))
    return current


def apply_appimage(downloaded: Path, current: Path, old_tag: str, new_tag: str) -> Path:
    """Poe o AppImage baixado (na MESMA pasta do atual) no lugar. O que esta
    rodando segue montado ate fechar: trocar o arquivo e seguro."""
    current = Path(os.path.realpath(current))
    target = appimage_target(current, old_tag, new_tag)
    try:
        os.chmod(downloaded, os.stat(downloaded).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        os.replace(downloaded, target)
        if target != current:
            current.unlink(missing_ok=True)
    except OSError as e:
        raise UpdateError("write", e.strerror or str(e))
    return target


def relaunch_appimage(path: Path, args=()):
    """Abre o AppImage novo, sem o ambiente que o antigo montou."""
    env = dict(os.environ)
    for k in ("APPIMAGE", "APPDIR", "ARGV0", "OWD", "DW4_APP_DIR"):
        env.pop(k, None)
    orig = env.pop("LD_LIBRARY_PATH_ORIG", None)       # o do usuario, antes do PyInstaller
    if orig is not None:
        env["LD_LIBRARY_PATH"] = orig
    else:
        env.pop("LD_LIBRARY_PATH", None)
    env["PYINSTALLER_RESET_ENVIRONMENT"] = "1"         # o novo e outra instancia, nao um filho
    subprocess.Popen([str(path), *args], env=env, cwd=str(Path(path).parent),
                     start_new_session=True, close_fds=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ── Windows: pacote .zip ─────────────────────────────────────────────────────
def stage_windows(zip_path: Path, stage: Path) -> Path:
    """Extrai o pacote em `stage` (apagada antes) e devolve a pasta do pacote
    la dentro (a que tem DigimonWorld4-Recomp.exe)."""
    stage = Path(stage)
    try:
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir(parents=True)
        base = os.path.realpath(stage)
        with zipfile.ZipFile(zip_path) as z:
            for info in z.infolist():
                name = info.filename.replace("\\", "/")
                parts = [p for p in name.split("/") if p]
                # so caminhos relativos dentro de stage: nada de "..", absolutos, C: ou links
                if (not parts or name.startswith("/") or any(p == ".." or ":" in p for p in parts)
                        or (info.external_attr >> 16) & 0o170000 == 0o120000):
                    raise UpdateError("bad", "nome invalido no pacote: " + info.filename)
                out = os.path.realpath(os.path.join(base, *parts))
                if os.path.commonpath([base, out]) != base:
                    raise UpdateError("bad", "nome invalido no pacote: " + info.filename)
                if info.is_dir():
                    os.makedirs(_long(out), exist_ok=True)
                    continue
                os.makedirs(_long(os.path.dirname(out)), exist_ok=True)
                with z.open(info) as src, open(_long(out), "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
    except UpdateError:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    except (OSError, zipfile.BadZipFile) as e:
        shutil.rmtree(stage, ignore_errors=True)
        raise UpdateError("write", str(e))
    for root in [stage] + sorted(p for p in stage.iterdir() if p.is_dir()):
        if (root / WIN_STUB).is_file() and (root / "launcher" / "dw4_launcher.py").is_file():
            return root
    shutil.rmtree(stage, ignore_errors=True)
    raise UpdateError("bad", "o pacote nao tem " + WIN_STUB)


def start_windows_install(staged_root: Path, install_dir: Path, pid: int):
    """Abre o launcher NOVO (da pasta de preparo) para instalar sobre
    `install_dir` quando o processo `pid` (este launcher) fechar."""
    flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    subprocess.Popen([str(Path(staged_root) / WIN_STUB), "--install-update", str(install_dir), str(pid)],
                     cwd=str(staged_root), close_fds=True, creationflags=flags)


def wait_pid(pid: int, timeout: float = 60.0):
    """Espera o processo acabar (no maximo `timeout` s). So Windows."""
    if not WIN or pid <= 0:
        return
    import ctypes
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(0x00100000, False, int(pid))        # SYNCHRONIZE
    if h:
        k32.WaitForSingleObject(ctypes.c_void_p(h), int(timeout * 1000))
        k32.CloseHandle(ctypes.c_void_p(h))


def copy_over(src: Path, dst: Path, tries: int = 30, pause: float = 1.0, log=None) -> bool:
    """Copia a arvore `src` sobre `dst`, arquivo por arquivo (cada um entra por
    um ".dw4new" renomeado, para nunca ficar pela metade). Nada e apagado: o
    que so existe em `dst` (arquivos do usuario) fica. Um arquivo ainda em uso
    e tentado de novo `tries` vezes."""
    src, dst = Path(src), Path(dst)
    for base, dirs, files in os.walk(_long(src)):
        dirs.sort()
        rel = Path(os.path.relpath(base, _long(src)))
        try:
            os.makedirs(_long(dst / rel), exist_ok=True)
        except OSError as e:
            if log:
                log("mkdir %s: %s" % (rel, e))
            return False
        for name in sorted(files):
            out, tmp = dst / rel / name, dst / rel / (name + ".dw4new")
            for attempt in range(tries):
                try:
                    shutil.copy2(_long(Path(base) / name), _long(tmp))
                    os.replace(_long(tmp), _long(out))
                    break
                except OSError as e:
                    if log and attempt in (0, tries - 1):
                        log("%s: %s (tentativa %d)" % (rel / name, e, attempt + 1))
                    time.sleep(pause)
            else:
                try:
                    tmp.unlink()
                except OSError:
                    pass
                return False
    return True


def install_windows(staged_root: Path, install_dir: Path, pid: int, log=None,
                    tries: int = 30, pause: float = 1.0) -> bool:
    """O passo do ajudante (--install-update): espera o launcher antigo fechar e
    copia o pacote novo sobre a instalacao."""
    wait_pid(pid)
    time.sleep(min(pause, 1.0))         # o .exe de entrada antigo sai logo depois do Python dele
    return copy_over(staged_root, install_dir, tries, pause, log)


def run_install_helper(argv) -> int:
    """`dw4_launcher.py --install-update <pasta instalada> <pid> [args do launcher]`,
    rodando do pacote NOVO na pasta de preparo. Depois abre o launcher instalado."""
    i = argv.index("--install-update")
    target, pid, extra = Path(argv[i + 1]), int(argv[i + 2]), list(argv[i + 3:])
    staged = Path(__file__).resolve().parent.parent
    # O log fica com os outros do launcher (a pasta de preparo e apagada depois).
    data = os.environ.get("DW4_DATA_DIR") or os.path.join(os.environ.get("APPDATA") or str(staged.parent), "dw4-recomp")
    log_path = Path(data) / "logs" / "update-install.log"
    lines = []

    def log(msg):
        lines.append("%s %s" % (time.strftime("%H:%M:%S"), msg))

    ok = False
    try:
        if not (target / WIN_STUB).is_file() or not (staged / WIN_STUB).is_file() or staged == target:
            log("pastas invalidas: %s -> %s" % (staged, target))
        else:
            log("instalando %s sobre %s" % (staged, target))
            ok = install_windows(staged, target, pid, log)
    except Exception as e:              # o ajudante nao tem janela: tudo vai para o log e para o aviso
        log("erro: %r" % (e,))
    log("resultado: %s" % ("ok" if ok else "FALHOU"))
    try:
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    except OSError:
        pass
    if not ok and WIN and not extra:
        import ctypes
        ctypes.windll.user32.MessageBoxW(
            None,
            "Não foi possível instalar a atualização. Baixe a versão nova na página do projeto:\n"
            "Could not install the update. Download the new version from the project page:\n\n"
            + RELEASES_PAGE, "Digimon World 4 - Recomp", 0x10)
        try:
            os.startfile(RELEASES_PAGE)         # noqa: S606  (endereco fixo do projeto)
        except OSError:
            pass
    stub = target / WIN_STUB
    if stub.is_file():
        try:
            flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            subprocess.Popen([str(stub), *extra], cwd=str(target), close_fds=True, creationflags=flags)
        except OSError:
            pass
    return 0 if ok else 1


def cleanup(update_dir: Path):
    """Apaga restos de uma atualizacao anterior (pasta de preparo, .part).
    Nunca a pasta de onde este proprio launcher esta rodando."""
    update_dir = Path(update_dir)
    try:
        here = Path(__file__).resolve()
        if update_dir.resolve() in here.parents:
            return
    except OSError:
        return
    shutil.rmtree(update_dir, ignore_errors=True)


if __name__ == "__main__" and "--install-update" in sys.argv:
    sys.exit(run_install_helper(sys.argv))
