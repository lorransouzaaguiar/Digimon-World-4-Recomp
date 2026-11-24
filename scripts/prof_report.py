#!/usr/bin/env python3
"""
Resumo de um perfil gravado com DW4_PROF=<arquivo> (src/hooks/prof.c).

    scripts/soak.py -e DW4_PROF=/tmp/p.txt -e DW4_PROF_FROM=3000
    scripts/prof_report.py /tmp/p.txt [--top 40] [--thread NOME]

Mostra o consumo de CPU por thread e, em cada uma, as funcoes mais amostradas
(simbolos via `nm`; sub_XXXXXXXX = codigo do jogo).
"""
import argparse
import bisect
import collections
import os
import subprocess
import sys


def load_syms(path):
    out = []
    for args in (["nm", "-n", "--defined-only", path], ["nm", "-n", "-D", "--defined-only", path]):
        try:
            txt = subprocess.run(args, capture_output=True, text=True).stdout
        except OSError:
            continue
        for ln in txt.splitlines():
            p = ln.split()
            if len(p) == 3 and p[1] in "tTwW":
                out.append((int(p[0], 16), p[2]))
    out.sort()
    return [a for a, _ in out], [n for _, n in out]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--top", type=int, default=30)
    ap.add_argument("--thread", help="so threads cujo nome contem isto")
    args = ap.parse_args()

    maps, threads, samples, flips = [], {}, [], 0
    for ln in open(args.file):
        if ln.startswith("M "):
            p = ln.split()
            lo, hi = (int(x, 16) for x in p[1].split("-"))
            maps.append((lo, hi, int(p[3], 16), p[6] if len(p) > 6 else "?"))
        elif ln.startswith("T "):
            p = ln.split(None, 2)
            threads[int(p[1])] = p[2].strip()
        elif ln.startswith("F "):
            flips = int(ln.split()[1])
        elif ln.startswith("S "):
            p = ln.split()
            samples.append((int(p[1]), int(p[2], 16)))
    maps.sort()
    symtab = {}

    def resolve(rip):
        for lo, hi, off, path in maps:
            if lo <= rip < hi:
                if path not in symtab:
                    symtab[path] = load_syms(path) if os.path.exists(path) else ([], [])
                addrs, names = symtab[path]
                base = os.path.basename(path)
                if not addrs:
                    return base, base
                # ET_DYN: endereco do arquivo = rip - lo + off (bias 0 no nm)
                for v in (rip - lo + off, rip):
                    i = bisect.bisect_right(addrs, v) - 1
                    if i >= 0 and v - addrs[i] < 0x40000:
                        return base, names[i]
                return base, base
        return "?", "?"

    total = len(samples)
    by_thread = collections.Counter(t for t, _ in samples)
    print(f"{total} amostras, {len(by_thread)} threads, ate o flip {flips}")
    for tid, n in by_thread.most_common():
        name = threads.get(tid, "?")
        if args.thread and args.thread not in name:
            continue
        if n < total * 0.005:
            continue
        print(f"\n== thread {tid} [{name}]: {n} amostras ({100.0 * n / total:.1f}% da CPU)")
        mods, funcs = collections.Counter(), collections.Counter()
        for t, rip in samples:
            if t != tid:
                continue
            m, f = resolve(rip)
            if f.startswith("sub_") and m.startswith("dw4_recomp"):
                mods["(codigo do jogo)"] += 1
            else:
                mods[m] += 1
            funcs[(m, f)] += 1
        print("   por modulo: " + ", ".join(f"{m} {100.0 * c / n:.0f}%" for m, c in mods.most_common(8)))
        for (m, f), c in funcs.most_common(args.top):
            print(f"   {100.0 * c / n:5.1f}%  {c:7d}  {f}  [{m}]")


if __name__ == "__main__":
    sys.exit(main())
