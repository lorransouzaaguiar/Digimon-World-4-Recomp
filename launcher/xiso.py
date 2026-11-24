"""Leitor minimo de imagens de disco do Xbox (XDVDFS / "XISO").

So o necessario para copiar arquivos da raiz do disco para uma pasta: acha o
volume (imagem so da particao do jogo ou imagem completa estilo Redump),
percorre a arvore do diretorio raiz e copia os arquivos pedidos.

Formato (publico, documentado por extract-xiso e xdvdfs):
  setor = 2048 bytes; descritor do volume no setor 32 da particao:
    "MICROSOFT*XBOX*MEDIA" | setor da raiz (u32) | tamanho da raiz (u32) | ...
  entrada de diretorio (arvore binaria, deslocamentos em unidades de 4 bytes):
    esquerda (u16) | direita (u16) | setor inicial (u32) | tamanho (u32) |
    atributos (u8) | tamanho do nome (u8) | nome
"""
import struct
from pathlib import Path

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Inicio da particao do jogo dentro da imagem: 0 = imagem so da particao;
# os outros = imagens completas (XGD1 do Xbox original, e XGD2/XGD3 por garantia).
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)
ATTR_DIRECTORY = 0x10


class XisoError(Exception):
    pass


def _find_volume(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        head = f.read(28)
        if len(head) == 28 and head[:20] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", head, 20)
            return base, root_sector, root_size
    raise XisoError("not an Xbox disc image")


def list_root(path):
    """{nome em minusculas: (nome, deslocamento em bytes, tamanho)} da raiz do disco."""
    with open(path, "rb") as f:
        base, root_sector, root_size = _find_volume(f)
        if not 0 < root_size <= 16 * 1024 * 1024:
            raise XisoError("bad root directory size")
        f.seek(base + root_sector * SECTOR)
        table = f.read(root_size)
    out, todo, seen = {}, [0], set()
    while todo:
        off = todo.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:                    # preenchimento de fim de setor
            continue
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        if name and not attr & ATTR_DIRECTORY:
            out[name.lower()] = (name, base + sector * SECTOR, size)
        for child in (left, right):
            if child and child != 0xFFFF:
                todo.append(child * 4)
    return out


def extract(path, names, dest, progress=None):
    """Copia `names` (sem diferenciar maiusculas) da raiz da imagem para `dest`.
    Devolve a lista dos que faltam na imagem (nada e copiado se faltar algum)."""
    root = list_root(path)
    missing = [n for n in names if n.lower() not in root]
    if missing:
        return missing
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    total = sum(root[n.lower()][2] for n in names) or 1
    done = 0
    with open(path, "rb") as f:
        for n in names:
            _, off, size = root[n.lower()]
            tmp = dest / (n.lower() + ".part")
            f.seek(off)
            left = size
            with open(tmp, "wb") as o:
                while left:
                    chunk = f.read(min(left, 4 << 20))
                    if not chunk:
                        tmp.unlink(missing_ok=True)
                        raise XisoError("image is truncated")
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
                    if progress:
                        progress(done / total)
            tmp.replace(dest / n.lower())
    return []
