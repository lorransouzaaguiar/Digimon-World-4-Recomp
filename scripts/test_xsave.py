#!/usr/bin/env python3
"""Testes da importacao de saves de Xbox (launcher/xsave.py), sem janela e sem
tocar nos saves de verdade: os saves sao sinteticos (bytes aleatorios assinados
com uma chave qualquer, como viriam de um console) e tudo acontece numa pasta
temporaria.

    scripts/test_xsave.py              (~2 s)

Cobre: assinatura (confere / nao confere / tamanho errado); leitura do .zip nos
formatos em que os saves circulam (UDATA na raiz, dentro de outra pasta, com
barras do Windows, caixa trocada); o que tem de ser recusado (outro jogo, sem
SaveData.dat, arquivo truncado, .zip estragado, caminho com ".."); e o fluxo
pelo Backend do launcher: backup dos saves atuais, troca so dos arquivos do
save, e o "Importar .zip" de um backup do proprio launcher continuando igual.
"""
import hashlib
import hmac
import os
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "launcher"))
import xsave  # noqa: E402

FAILS = []
SLOT = "80482ED3557F"


def check(cond, what):
    print(("  ok    " if cond else "  FALHA ") + what)
    if not cond:
        FAILS.append(what)


def fake_save(key: bytes, seed: bytes) -> bytes:
    """Um SaveData.dat de mentira: 2 blocos, assinados com `key`."""
    out = b""
    for i in range(2):
        block = (hashlib.sha256(seed + bytes([i])).digest() * (xsave.BLOCK // 32 + 1))[:xsave.BLOCK]
        out += block + hmac.new(key, block, hashlib.sha1).digest()
    return out


def make_zip(path, entries):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return path


def code_of(fn):
    try:
        fn()
    except xsave.XSaveError as e:
        return e.code
    return None


def test_signature():
    console = fake_save(b"K" * 16, b"a")
    check(not xsave.signed_ok(console), "save assinado por um console nao confere com a chave do port")
    fixed = xsave.resign(console)
    check(xsave.signed_ok(fixed), "depois de reassinar, confere")
    check(len(fixed) == len(console) and all(
        fixed[o:o + xsave.BLOCK] == console[o:o + xsave.BLOCK]
        for o in range(0, len(console), xsave.BLOCK + xsave.SIG)), "reassinar nao muda o conteudo dos blocos")
    check(xsave.resign(fixed) == fixed, "reassinar um save ja do port nao muda nada")
    check(code_of(lambda: xsave.resign(console[:-1])) == "bad", "tamanho errado e recusado")
    check(code_of(lambda: xsave.resign(b"")) == "bad", "arquivo vazio e recusado")
    check(not xsave.signed_ok(b"") and not xsave.signed_ok(fixed[:-1]), "signed_ok: vazio e truncado nao conferem")


def test_zip(tmp):
    console = fake_save(b"K" * 16, b"b")
    base = {"TitleMeta.xbx": b"tm", "TitleImage.xbx": b"ti", "SaveImage.xbx": b"si",
            SLOT + "/SaveMeta.xbx": b"sm", SLOT + "/SaveData.dat": console}
    for label, prefix in (("UDATA na raiz", "UDATA/42410002/"),
                          ("dentro de outra pasta", "meu save/copia/UDATA/42410002/"),
                          ("barras do Windows e caixa trocada", "Stuff\\udata\\42410002\\")):
        sep = "\\" if "\\" in prefix else "/"
        z = make_zip(tmp / "a.zip", {prefix + n.replace("/", sep): d for n, d in base.items()})
        got = xsave.read_zip(z)
        check(sorted(got) == sorted(base) and xsave.signed_ok(got[SLOT + "/SaveData.dat"])
              and got["TitleMeta.xbx"] == b"tm", "le o .zip: " + label)
    extra = {"UDATA/42410002/" + n: d for n, d in base.items()}
    extra.update({"UDATA/42410002/datelinfo.xbx": b"x", "UDATA/42410002/../../fora.txt": b"x",
                  "UDATA/42410002/" + SLOT + "/sub/outro.bin": b"x", "leia-me.txt": b"x",
                  "UDATA/42410002/naoehex12ch/SaveData.dat": console})
    got = xsave.read_zip(make_zip(tmp / "b.zip", extra))
    check(sorted(got) == sorted(base), "arquivos fora do formato (inclusive com '..') sao ignorados")
    low = xsave.read_zip(make_zip(tmp / "c.zip", {"UDATA/42410002/80482ed3557f/savedata.dat": console}))
    check(list(low) == [SLOT + "/savedata.dat"], "pasta do save em minusculas vira maiusculas")
    check(code_of(lambda: xsave.read_zip(make_zip(tmp / "d.zip", {"UDATA/4D530004/" + SLOT + "/SaveData.dat": console})))
          == "notxbox", "save de outro jogo: nao e deste jogo")
    check(code_of(lambda: xsave.read_zip(make_zip(tmp / "e.zip", {"UDATA/42410002/TitleMeta.xbx": b"tm"})))
          == "notxbox", "sem SaveData.dat: nao e um save")
    check(code_of(lambda: xsave.read_zip(make_zip(tmp / "f.zip", {"UDATA/42410002/" + SLOT + "/SaveData.dat": console[:1000]})))
          == "bad", "SaveData.dat truncado: estragado")
    (tmp / "g.zip").write_bytes(b"isto nao e um zip")
    check(code_of(lambda: xsave.read_zip(tmp / "g.zip")) == "bad", ".zip estragado")
    check(code_of(lambda: xsave.read_zip(tmp / "nao-existe.zip")) == "bad", "arquivo que nao existe")


def test_backend(tmp):
    """O caminho inteiro pelo Backend (Qt fora da tela), numa pasta de dados de teste."""
    data = tmp / "data"
    os.environ["DW4_DATA_DIR"] = str(data)
    os.environ["QT_QPA_PLATFORM"] = "offscreen"
    try:
        import dw4_launcher as L
        from PySide6.QtCore import QUrl
        from PySide6.QtGui import QGuiApplication
    except ImportError as e:
        print("  --    Backend: PySide6 ausente (%s), pulado" % e)
        return
    app = QGuiApplication.instance() or QGuiApplication(["test"])  # noqa: F841
    b = L.Backend()
    msgs = []
    b.message.connect(msgs.append)
    t = L.STR[b._lang()]
    slot = data / "save_data" / "UDATA" / "42410002" / SLOT
    slot.mkdir(parents=True)
    mine = xsave.resign(fake_save(b"M" * 16, b"meu"))
    (slot / "SaveData.dat").write_bytes(mine)
    (data / "save_data" / "outro.bin").write_bytes(b"fica")

    console = fake_save(b"K" * 16, b"c")
    z = make_zip(tmp / "xbox.zip", {"UDATA/42410002/" + SLOT + "/SaveData.dat": console,
                                    "UDATA/42410002/" + SLOT + "/SaveMeta.xbx": b"sm",
                                    "UDATA/42410002/datelinfo.xbx": b"x"})
    b.importSaves(QUrl.fromLocalFile(str(z)))
    now = (slot / "SaveData.dat").read_bytes()
    check(msgs and msgs[-1].startswith(t["import_xbox_done"]), "Backend: save de Xbox importado")
    check(xsave.signed_ok(now) and now == xsave.resign(console), "Backend: o save gravado e o do .zip, assinado para o port")
    check((data / "save_data" / "outro.bin").read_bytes() == b"fica" and not (slot.parent / "datelinfo.xbx").exists(),
          "Backend: o resto de save_data fica como estava")
    backups = sorted((data / "backups").glob("saves-*.zip"))
    ok = False
    if backups:
        with zipfile.ZipFile(backups[-1]) as bz:
            ok = bz.read("save_data/UDATA/42410002/" + SLOT + "/SaveData.dat") == mine
    check(ok, "Backend: o save anterior foi para o backup")

    for name, content, key in (("ruim.zip", {"leia-me.txt": b"x"}, "import_bad"),
                               ("trunc.zip", {"UDATA/42410002/" + SLOT + "/SaveData.dat": console[:99]}, "import_xbox_bad")):
        b.importSaves(QUrl.fromLocalFile(str(make_zip(tmp / name, content))))
        check(msgs[-1] == t[key] and (slot / "SaveData.dat").read_bytes() == now,
              "Backend: %s recusado sem tocar no save" % name)

    # um backup do proprio launcher continua voltando como antes
    # (copia com outro nome: o backup novo, feito no mesmo segundo, teria o mesmo nome)
    old = tmp / "meu-backup.zip"
    old.write_bytes(backups[-1].read_bytes())
    b.importSaves(QUrl.fromLocalFile(str(old)))
    check(msgs[-1].startswith(t["import_done"]) and (slot / "SaveData.dat").read_bytes() == mine,
          "Backend: importar o backup do launcher devolve o save anterior")


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        test_signature()
        test_zip(tmp)
        test_backend(tmp)
    print("FALHOU: %d" % len(FAILS) if FAILS else "xsave ok")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
