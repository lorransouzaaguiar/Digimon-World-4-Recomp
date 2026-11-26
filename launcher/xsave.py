"""Saves de Xbox do Digimon World 4 (um save copiado de um console, de um
emulador ou baixado de um site) para o formato que o port aceita. Sem Qt.

O jogo assina o save com XCalculateSignature: HMAC-SHA1 cuja chave o kernel do
Xbox deriva do certificado do jogo (XboxSignatureKey). O port nao tem essa
chave e usa 16 bytes zerados, entao um save vindo de um Xbox de verdade tem
assinatura que nao confere e o jogo diz "This game is corrupted and cannot be
used". O conteudo e o mesmo: basta assinar de novo com a chave do port.

SaveData.dat (65584 bytes) sao blocos de 32772 bytes de dados seguidos de 20
bytes de assinatura, HMAC-SHA1 do bloco (medido no port, PORT_NOTES 8).

Um .zip de save de Xbox traz a pasta UDATA/<title id>/, as vezes dentro de
outras pastas:
    UDATA/42410002/TitleMeta.xbx, TitleImage.xbx, SaveImage.xbx
    UDATA/42410002/<12 hex>/SaveMeta.xbx, SaveData.dat
"""
import hashlib
import hmac
import zipfile
from pathlib import PurePosixPath

TITLE_ID = "42410002"
BLOCK, SIG = 32772, 20
PORT_KEY = bytes(16)
MAX_FILE = 4 * 1024 * 1024          # nenhum arquivo de save chega perto disto
MAX_FILES = 64


class XSaveError(Exception):
    """code: 'notxbox' (o .zip nao tem save deste jogo), 'bad' (tem, mas esta
    estragado ou fora do formato esperado)."""

    def __init__(self, code):
        super().__init__(code)
        self.code = code


def signed_ok(data: bytes) -> bool:
    """Todas as assinaturas de um SaveData.dat conferem com a chave do port."""
    if not data or len(data) % (BLOCK + SIG):
        return False
    return all(hmac.new(PORT_KEY, data[o:o + BLOCK], hashlib.sha1).digest() == data[o + BLOCK:o + BLOCK + SIG]
               for o in range(0, len(data), BLOCK + SIG))


def resign(data: bytes) -> bytes:
    """O mesmo SaveData.dat assinado com a chave do port."""
    if not data or len(data) % (BLOCK + SIG):
        raise XSaveError("bad")
    out = bytearray()
    for o in range(0, len(data), BLOCK + SIG):
        block = data[o:o + BLOCK]
        out += block + hmac.new(PORT_KEY, block, hashlib.sha1).digest()
    return bytes(out)


def _is_slot(name: str) -> bool:
    return len(name) == 12 and all(c in "0123456789abcdefABCDEF" for c in name)


def read_zip(path) -> dict:
    """Le um .zip de save de Xbox. Devolve {caminho relativo a UDATA/<title
    id>/: bytes}, com cada SaveData.dat ja assinado para o port. So entram os
    arquivos que o jogo usa; o resto do .zip e ignorado."""
    out = {}
    try:
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                if info.is_dir():
                    continue
                parts = PurePosixPath(info.filename.replace("\\", "/")).parts
                low = [p.lower() for p in parts]
                at = next((i for i in range(len(low) - 1)
                           if low[i] == "udata" and low[i + 1] == TITLE_ID), None)
                if at is None:
                    continue
                rel = parts[at + 2:]
                # so <arquivo>.xbx na raiz do titulo e <slot>/SaveMeta.xbx|SaveData.dat
                if len(rel) == 1 and rel[0].lower() in ("titlemeta.xbx", "titleimage.xbx", "saveimage.xbx"):
                    name = rel[0]
                elif len(rel) == 2 and _is_slot(rel[0]) and rel[1].lower() in ("savemeta.xbx", "savedata.dat"):
                    name = rel[0].upper() + "/" + rel[1]
                else:
                    continue
                if info.file_size > MAX_FILE or len(out) >= MAX_FILES:
                    raise XSaveError("bad")
                data = z.read(info)
                if name.lower().endswith("savedata.dat"):
                    data = resign(data)
                out[name] = data
    except (OSError, zipfile.BadZipFile, RuntimeError, ValueError):
        raise XSaveError("bad")
    if not any(n.lower().endswith("savedata.dat") for n in out):
        raise XSaveError("notxbox")
    return out
