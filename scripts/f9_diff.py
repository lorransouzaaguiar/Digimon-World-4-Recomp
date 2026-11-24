#!/usr/bin/env python3
"""
Analisa uma captura F9 (build/shots/f9-NNN.txt): compara o frame A (normal)
com o frame B (sem os draws so de profundidade) e mostra o que esta sendo
escondido e por quem.

    scripts/f9_diff.py                      # a captura mais recente
    scripts/f9_diff.py build/shots/f9-003.txt

Saida:
  ESCONDIDOS  draws de cor que pintaram muito mais pixels em B do que em A
  SUSPEITOS   draws so de profundidade do frame A cuja caixa na tela cruza a
              de um escondido e que estao na frente (z menor), com o estado
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RX = re.compile(r"\[Q\] f(\d+) ([AB]) ([nd])=(\d+) surf=([0-9A-F]+) bbox=\(([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)\) "
                r"zn=\(([-\d.einf]+),([-\d.einf]+)\) samples=(\d+) (.*)")


def parse(path):
    frames = {}
    for line in open(path, errors="replace"):
        m = RX.search(line)
        if not m:
            continue
        f, ab, kind, n = int(m.group(1)), m.group(2), m.group(3), int(m.group(4))
        d = dict(frame=f, ab=ab, kind=kind, n=n, surf=m.group(5),
                 bbox=tuple(float(m.group(i)) for i in range(6, 10)),
                 z=(float(m.group(10)), float(m.group(11))),
                 samples=int(m.group(12)), state=m.group(13).strip())
        frames.setdefault((f, ab), []).append(d)
    return frames


def overlap(a, b):
    return a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    path = args[0] if args else max(glob.glob(os.path.join(ROOT, "build/shots/f9-*.txt")),
                                    key=os.path.getmtime)
    frames = parse(path)
    fa = [k for k in frames if k[1] == "A"]
    fb = [k for k in frames if k[1] == "B"]
    if not fa or not fb:
        print(f"{path}: captura sem frames A e B completos ({len(frames)} frames)")
        return 1
    # o frame A inteiro e o que antecede imediatamente o B
    kb = max(fb)
    ka = max(k for k in fa if k[0] < kb[0])
    A, B = frames[ka], frames[kb]
    print(f"{path}\nframe A f{ka[0]}: {len(A)} draws ({sum(d['kind']=='d' for d in A)} so de profundidade)"
          f"   frame B f{kb[0]}: {len(B)} draws")

    main_surf = max(set(d["surf"] for d in A), key=lambda s: sum(d["surf"] == s for d in A))
    bcol = {d["n"]: d for d in B if d["kind"] == "n"}
    hidden = []
    for d in A:
        if d["kind"] != "n" or d["surf"] != main_surf:
            continue
        e = bcol.get(d["n"])
        if e and e["state"] == d["state"] and e["samples"] > d["samples"] + 40 \
                and e["samples"] > 1.5 * d["samples"]:
            hidden.append((d, e))
    print(f"\nESCONDIDOS ({len(hidden)}): n  pixels A -> B  caixa  z")
    for d, e in sorted(hidden, key=lambda p: p[0]["samples"] - p[1]["samples"])[:25]:
        print(f"  n={d['n']:<5} {d['samples']:>7} -> {e['samples']:<7} "
              f"({d['bbox'][0]:.0f},{d['bbox'][1]:.0f})-({d['bbox'][2]:.0f},{d['bbox'][3]:.0f}) "
              f"z={d['z'][0]:.5f}..{d['z'][1]:.5f}\n      {d['state']}")

    deps = [d for d in A if d["kind"] == "d" and d["surf"] == main_surf and d["samples"] > 0]
    sus = {}
    for d, _ in hidden:
        for o in deps:
            if overlap(d["bbox"], o["bbox"]) and o["z"][0] <= d["z"][1]:
                sus.setdefault(o["n"], [o, 0])[1] += 1
    print(f"\nSUSPEITOS ({len(sus)} de {len(deps)} draws so de profundidade com pixels): "
          "d  pixels  escondidos-cruzados  caixa  z")
    for n, (o, c) in sorted(sus.items(), key=lambda kv: -kv[1][1])[:25]:
        print(f"  d={n:<5} {o['samples']:>7}  x{c:<3} "
              f"({o['bbox'][0]:.0f},{o['bbox'][1]:.0f})-({o['bbox'][2]:.0f},{o['bbox'][3]:.0f}) "
              f"z={o['z'][0]:.5f}..{o['z'][1]:.5f}\n      {o['state']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
