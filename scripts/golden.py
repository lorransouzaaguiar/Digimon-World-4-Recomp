#!/usr/bin/env python3
"""
Compara capturas de um run com os frames de referencia em tests/golden/.

    scripts/golden.py build/soak/01/shots             # compara
    scripts/golden.py build/soak/01/shots --update    # adota como referencia
    scripts/golden.py a.bmp b.bmp                     # compara dois arquivos

Comparacao tolerante (os fundos dos menus sao animados): a imagem e reduzida a
uma grade 32x24 de cor media e mede-se a diferenca media por celula (0-255).
Passa se a media <= --tol (padrao 10) e nenhuma celula difere mais que --cell
(padrao 90). Um frame branco, preto ou com as cores trocadas estoura as duas;
uma animacao em outra fase nao.

So BMP 24 bits 640x480 (o que DW4_SHOT e "shot:" gravam). Sem dependencias.
"""
import os
import shutil
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GW, GH = 32, 24


def load_grid(path):
    data = open(path, "rb").read()
    if data[:2] != b"BM":
        raise ValueError("nao e BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError("so BMP de 24 bits")
    row = (w * 3 + 3) & ~3
    cw, ch = w // GW, h // GH
    grid = []
    for gy in range(GH):
        for gx in range(GW):
            acc = [0, 0, 0]
            n = 0
            for y in range(gy * ch, (gy + 1) * ch, 4):
                base = off + (h - 1 - y) * row          # BMP e de baixo para cima
                for x in range(gx * cw, (gx + 1) * cw, 4):
                    p = base + x * 3
                    acc[0] += data[p]; acc[1] += data[p + 1]; acc[2] += data[p + 2]
                    n += 1
            grid.append(tuple(a // n for a in acc))
    return grid


def compare(a, b):
    ga, gb = load_grid(a), load_grid(b)
    diffs = [sum(abs(x - y) for x, y in zip(ca, cb)) / 3 for ca, cb in zip(ga, gb)]
    return sum(diffs) / len(diffs), max(diffs)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = sys.argv[1:]
    tol = float(next((o.split("=")[1] for o in opts if o.startswith("--tol=")), 10))
    cell = float(next((o.split("=")[1] for o in opts if o.startswith("--cell=")), 90))
    golden = os.path.join(ROOT, "tests", "golden")

    if len(args) == 2 and os.path.isfile(args[0]):
        mean, worst = compare(args[0], args[1])
        print(f"media {mean:.1f}  pior celula {worst:.1f}")
        return 0 if mean <= tol and worst <= cell else 1
    if len(args) != 1 or not os.path.isdir(args[0]):
        print(__doc__)
        return 2

    shots = sorted(f for f in os.listdir(args[0]) if f.endswith(".bmp"))
    if "--update" in opts:
        os.makedirs(golden, exist_ok=True)
        for f in shots:
            shutil.copy(os.path.join(args[0], f), os.path.join(golden, f))
        print(f"{len(shots)} frame(s) de referencia atualizados em tests/golden/")
        return 0

    bad = 0
    for f in shots:
        ref = os.path.join(golden, f)
        if not os.path.exists(ref):
            print(f"  ?      {f}: sem referencia (use --update)")
            continue
        mean, worst = compare(os.path.join(args[0], f), ref)
        ok = mean <= tol and worst <= cell
        bad += not ok
        print(f"  {'ok ' if ok else 'DIF'}    {f}: media {mean:.1f}  pior celula {worst:.1f}")
    missing = [g for g in sorted(os.listdir(golden)) if g.endswith(".bmp") and g not in shots] \
        if os.path.isdir(golden) else []
    for g in missing:
        print(f"  FALTA  {g}: o run nao gravou esta captura")
    bad += len(missing)
    print("goldens: " + ("todos conferem" if not bad else f"{bad} diferenca(s)"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
