#!/usr/bin/env python3
"""Testes da atualizacao do launcher (launcher/updater.py), sem janela e sem
tocar em nenhuma instalacao de verdade: um servidor HTTP local faz as vezes do
GitHub e tudo acontece numa pasta temporaria.

    scripts/test_updater.py            tudo offline (~3 s)
    scripts/test_updater.py --online   tambem consulta o release de verdade no GitHub

Cobre: comparacao de versoes; leitura do release (hash pelo campo "digest" e
pelo arquivo SHA256SUMS, rascunho, asset de outro endereco); download com hash
certo, errado e cancelado; troca do AppImage; extracao do .zip (inclusive um
.zip malicioso); copia sobre a instalacao do Windows preservando arquivos do
usuario; e o fluxo inteiro pelo Backend do launcher (threads e sinais do Qt),
nos dois tipos de instalacao.
"""
import functools
import hashlib
import http.server
import json
import os
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "launcher"))
import updater  # noqa: E402

FAILS = []


def check(cond, what):
    print(("  ok    " if cond else "  FALHA ") + what)
    if not cond:
        FAILS.append(what)


def raises(code, fn, what):
    try:
        fn()
    except updater.UpdateError as e:
        check(e.code == code, "%s -> erro '%s'" % (what, e.code))
    else:
        check(False, what + " (nao deu erro)")


def serve(folder):
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass

    class Server(http.server.ThreadingHTTPServer):
        def handle_error(self, request, client_address):    # o teste de cancelar corta a conexao
            pass

    srv = Server(("127.0.0.1", 0), functools.partial(Quiet, directory=str(folder)))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d/" % srv.server_address[1]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_release(web, base, tag, files, digest=True, name="latest", **extra):
    """files: {nome do asset: bytes}. Grava os assets e o JSON como o da API."""
    assets = []
    for n, data in files.items():
        (web / n).write_bytes(data)
        a = {"name": n, "size": len(data), "browser_download_url": base + n}
        if digest:
            a["digest"] = "sha256:" + sha(data)
        assets.append(a)
    d = {"tag_name": tag, "draft": False, "prerelease": False, "html_url": base + "notas", "assets": assets}
    d.update(extra)
    (web / name).write_text(json.dumps(d))
    return base + name


def win_zip(tag, extra=()):
    top = "DigimonWorld4-Recomp-%s-windows-x86_64" % tag
    import io
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        z.writestr(top + "/DigimonWorld4-Recomp.exe", b"stub " + tag.encode())
        z.writestr(top + "/launcher/dw4_launcher.py", b"# launcher " + tag.encode())
        z.writestr(top + "/launcher/VERSION", tag.encode() + b"\n")
        z.writestr(top + "/game/dw4_recomp.exe", b"jogo " + tag.encode())
        for n, data in extra:
            z.writestr(n, data)
    return buf.getvalue()


def unit(tmp):
    print("versoes")
    check(updater.parse_version("v0.4.1") == (0, 4, 1) and updater.parse_version("dev") is None
          and updater.parse_version("v1.0-rc1") is None, "parse_version")
    check(updater.is_newer("v0.10.0", "v0.9.9") and not updater.is_newer("v0.4.1", "v0.4.1")
          and not updater.is_newer("v0.4.0", "v0.4.1") and not updater.is_newer("v9.9.9", "dev"), "is_newer")

    web = tmp / "web"
    web.mkdir()
    srv, base = serve(web)
    tag = "v9.9.9"
    app_name = "DigimonWorld4-Recomp-%s-x86_64.AppImage" % tag
    zip_name = "DigimonWorld4-Recomp-%s-windows-x86_64.zip" % tag
    app_data, zip_data = b"AppImage novo " * 50000, win_zip(tag)
    files = {app_name: app_data, zip_name: zip_data,
             "DigimonWorld4-Recomp-%s-linux-x86_64.tar.gz" % tag: b"tar"}

    print("leitura do release")
    api = write_release(web, base, tag, files)
    r = updater.check("appimage", api)
    check((r.tag, r.asset_name, r.size, r.sha256) == (tag, app_name, len(app_data), sha(app_data)), "AppImage pelo digest")
    r_win = updater.check("windows", api)
    check(r_win.asset_name == zip_name and r_win.sha256 == sha(zip_data), "zip de Windows pelo digest")
    check(updater.check("manual", api).asset_name == "" and updater.check("manual", api).tag == tag, "instalacao manual: so a versao")
    sums = "".join("%s  %s\n" % (sha(d), n) for n, d in files.items()).encode()
    api2 = write_release(web, base, tag, dict(files, **{"SHA256SUMS-%s.txt" % tag: sums}), digest=False, name="semdigest")
    check(updater.check("appimage", api2).sha256 == sha(app_data), "hash pelo SHA256SUMS quando nao ha digest")
    raises("bad", lambda: updater.check("appimage", write_release(web, base, tag, files, digest=False, name="semhash")),
           "release sem hash nenhum")
    raises("bad", lambda: updater.check("appimage", write_release(web, base, tag, files, name="rascunho", draft=True)),
           "rascunho")
    raises("bad", lambda: updater.check("appimage", write_release(web, base, "nightly", files, name="semversao")),
           "tag sem versao")
    d = json.loads((web / "latest").read_text())
    for a in d["assets"]:
        a["browser_download_url"] = "http://exemplo.invalid/" + a["name"]
    (web / "fora").write_text(json.dumps(d))
    check(updater.check("appimage", base + "fora").asset_name == "", "asset de outro endereco e ignorado")
    (web / "lixo").write_text("<html>")
    raises("bad", lambda: updater.check("appimage", base + "lixo"), "resposta que nao e JSON")
    raises("http", lambda: updater.check("appimage", base + "nao-existe"), "404")
    raises("net", lambda: updater.check("appimage", "http://127.0.0.1:9/latest", timeout=3), "servidor fora do ar")

    print("download")
    dl = tmp / "dl"
    seen = []
    got = updater.download(r, dl / app_name, seen.append)
    check(got.read_bytes() == app_data and seen and seen[-1] == 1.0 and not list(dl.glob("*.part")), "download confere")
    bad = updater.Release(tag, "", app_name, r.asset_url, r.size, "0" * 64)
    raises("hash", lambda: updater.download(bad, dl / "errado"), "hash errado")
    check(not (dl / "errado").exists() and not list(dl.glob("*.part")), "hash errado nao deixa arquivo")
    short = updater.Release(tag, "", app_name, r.asset_url, r.size - 10, r.sha256)
    raises("hash", lambda: updater.download(short, dl / "maior"), "veio mais do que o anunciado")
    try:
        updater.download(r, dl / "cancelado", cancelled=lambda: True)
        check(False, "cancelar")
    except updater.Cancelled:
        check(not (dl / "cancelado").exists() and not list(dl.glob("*.part")), "cancelar nao deixa arquivo")

    print("AppImage")
    inst = tmp / "apps"
    inst.mkdir()
    cur = inst / "DigimonWorld4-Recomp-v0.4.1-x86_64.AppImage"
    cur.write_bytes(b"antigo")
    new = updater.apply_appimage(updater.download(r, inst / ".dw4-update-x"), cur, "v0.4.1", tag)
    check(new.name == app_name and new.read_bytes() == app_data and not cur.exists()
          and os.access(new, os.X_OK) and sorted(p.name for p in inst.iterdir()) == [app_name],
          "troca o arquivo, com a versao nova no nome e executavel")
    ren = inst / "dw4.AppImage"
    ren.write_bytes(b"antigo")
    new = updater.apply_appimage(updater.download(r, inst / ".dw4-update-y"), ren, "v0.4.1", tag)
    check(new == ren and ren.read_bytes() == app_data, "arquivo renomeado pelo usuario mantem o nome")

    print("Windows")
    zp = updater.download(r_win, tmp / "upd" / zip_name)
    root = updater.stage_windows(zp, tmp / "upd" / "new")
    check((root / "DigimonWorld4-Recomp.exe").read_bytes() == b"stub " + tag.encode(), "extrai o pacote")
    for evil in ("../fora.txt", "/abs.txt", "C:/win.txt", "a/../../fora.txt"):
        (tmp / "evil.zip").write_bytes(win_zip(tag, [(evil, b"x")]))
        raises("bad", lambda: updater.stage_windows(tmp / "evil.zip", tmp / "evil"), "zip com nome '%s'" % evil)
        check(not (tmp / "fora.txt").exists() and not (tmp / "evil").exists(), "  nada escrito fora, preparo apagado")
    (tmp / "vazio.zip").write_bytes(b"nao e zip")
    raises("write", lambda: updater.stage_windows(tmp / "vazio.zip", tmp / "evil"), "arquivo que nao e zip")
    target = tmp / "Jogos" / "DW4"
    (target / "launcher").mkdir(parents=True)
    (target / "game" / "meu_disco").mkdir(parents=True)
    (target / "DigimonWorld4-Recomp.exe").write_bytes(b"stub antigo")
    (target / "launcher" / "VERSION").write_text("v0.4.1\n")
    (target / "game" / "meu_disco" / "default.xbe").write_bytes(b"do usuario")
    (target / "notas.txt").write_text("minhas")
    logs = []
    ok = updater.install_windows(root, target, 0, logs.append, tries=2, pause=0.01)
    check(ok and (target / "launcher" / "VERSION").read_text().strip() == tag
          and (target / "DigimonWorld4-Recomp.exe").read_bytes() == b"stub " + tag.encode()
          and (target / "game" / "dw4_recomp.exe").is_file(), "copia a versao nova sobre a instalacao")
    check((target / "game" / "meu_disco" / "default.xbe").read_bytes() == b"do usuario"
          and (target / "notas.txt").read_text() == "minhas" and not list(target.rglob("*.dw4new")),
          "arquivos do usuario ficam; nenhum temporario sobra")
    blocked = tmp / "bloqueado"
    blocked.mkdir()
    (blocked / "launcher").write_text("um arquivo onde devia haver uma pasta")
    check(not updater.install_windows(root, blocked, 0, logs.append, tries=2, pause=0.01), "destino impossivel -> falha, sem excecao")
    updater.cleanup(tmp / "upd")
    check(not (tmp / "upd").exists(), "limpa a pasta de preparo")
    srv.shutdown()
    return files, tag


def backend_flow(tmp, files, tag):
    """O caminho inteiro pelo Backend (Qt fora da tela), sem reabrir nada de verdade."""
    print("fluxo pelo launcher (Qt, fora da tela)")
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    try:
        import dw4_launcher as L
    except ImportError as e:
        print("  pulado: PySide6 nao disponivel (%s)" % e)
        return
    from PySide6.QtCore import QEventLoop, QTimer
    from PySide6.QtGui import QGuiApplication
    app = QGuiApplication.instance() or QGuiApplication([])
    web = tmp / "web2"
    web.mkdir()
    srv, base = serve(web)
    os.environ["DW4_UPDATE_API"] = write_release(web, base, tag, files)
    here = tmp / "here"
    here.mkdir()
    (here / "VERSION").write_text("v0.4.1\n")
    L.HERE, L.PACKAGED = here, True
    done = {}
    quits = []
    L.QGuiApplication.quit = staticmethod(lambda: quits.append(1))

    def run(b, start, until):
        loop, poll = QEventLoop(), QTimer()
        poll.timeout.connect(lambda: until() and loop.quit())
        poll.start(20)
        QTimer.singleShot(15000, loop.quit)
        start()
        if not until():
            loop.exec()
        poll.stop()

    for kind in ("appimage", "windows"):
        L.DATA = tmp / ("data-" + kind)
        L.APP = tmp / ("inst-" + kind) / "game"
        L.APP.mkdir(parents=True)
        cur = tmp / ("inst-" + kind) / "DigimonWorld4-Recomp-v0.4.1-x86_64.AppImage"
        cur.write_bytes(b"antigo")
        os.environ["APPIMAGE"] = str(cur)
        b = L.Backend()
        b._upd_kind = kind
        check(b.updateState == "idle" and b.updateText == "", "%s: comeca sem nada a dizer" % kind)
        run(b, b.autoCheckUpdate, lambda: b.updateState in ("available", "current", "error"))
        check(b.updateState == "available" and tag in b.updateText, "%s: consulta ao abrir acha a %s" % (kind, tag))
        b2 = L.Backend()
        check(b2.updateState == "available", "%s: a resposta fica guardada para a proxima abertura" % kind)
        b2.autoCheckUpdate()
        check(not b2._upd_busy, "%s: nao consulta de novo antes de 6 h" % kind)
        updater.relaunch_appimage = lambda p, args=(): done.__setitem__("appimage", p)
        updater.start_windows_install = lambda root, target, pid: done.__setitem__("windows", (root, target, pid))
        n = len(quits)
        run(b, b.installUpdate, lambda: len(quits) > n or b.updateState == "error")
        if kind == "appimage":
            p = done.get("appimage")
            check(p is not None and p.name.endswith("%s-x86_64.AppImage" % tag) and p.is_file() and not cur.exists()
                  and len(quits) == n + 1, "appimage: baixa, troca o arquivo, reabre o novo e fecha")
        else:
            root, target, pid = done.get("windows", (None, None, 0))
            check(root is not None and (root / "launcher" / "VERSION").read_text().strip() == tag
                  and target == L.APP.parent and pid == os.getpid() and len(quits) == n + 1
                  and not list((L.DATA / "update").glob("*.zip")),
                  "windows: baixa, prepara a pasta nova, chama o instalador e fecha")
    # erro de rede aparece como texto, sem excecao
    os.environ["DW4_UPDATE_API"] = "http://127.0.0.1:9/latest"
    b = L.Backend()
    b._upd_kind = "appimage"
    run(b, b.checkUpdate, lambda: b.updateState in ("available", "current", "error"))
    check(b.updateState == "error" and "GitHub" in b.updateText, "sem rede: mostra o erro")
    srv.shutdown()
    del app


def online():
    print("GitHub de verdade")
    for kind in ("appimage", "windows"):
        r = updater.check(kind)
        check(updater.parse_version(r.tag) is not None and r.asset_name.endswith(updater.ASSET_SUFFIX[kind])
              and len(r.sha256) == 64 and r.asset_url.startswith(updater.DOWNLOAD_PREFIX),
              "%s: %s, %s, %d bytes" % (kind, r.tag, r.asset_name, r.size))


def main():
    with tempfile.TemporaryDirectory(prefix="dw4-upd-test-") as t:
        tmp = Path(t)
        files, tag = unit(tmp)
        backend_flow(tmp, files, tag)
    if "--online" in sys.argv:
        online()
    print("FALHOU: %d" % len(FAILS) if FAILS else "updater ok")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
