#!/usr/bin/env python3
"""
Varredura estatica de ponteiros para codigo que o desmontador nao viu.

O Alchemy (engine do Digimon World 4) chama quase tudo por vtable ou por
tabelas de ponteiros registradas em .data. Nada da `call` nessas funcoes, entao
o detector do xboxrecomp nao as encontra, elas ficam sem entrada no dispatch e
o jogo loga `[ICALL] Failed to resolve VA ...` (ou, pior, pula a chamada).

Metodo (docs/pipeline/02-disassembly.md, "Reached only through a pointer"):
todo dword alinhado em todas as secoes que aponte para uma secao de codigo,
que nao seja inicio de funcao conhecido, que caia logo depois de padding
(int3/nop) ou de um `ret`/`ret N`/`jmp`, e que o proprio desmontador leia como
corpo de funcao.

Uso (de dentro de ./xboxrecomp, para o pacote `tools` ser importavel):

    ../.venv/bin/python ../scripts/scan_code_pointers.py ../game_data/Default.xbe \
        --functions ../analysis/disasm/functions.json \
        --seeds ../config/seed_functions.json

Depois: tools.disasm --seed-functions ../config/seed_functions.json e relift.
"""

import argparse
import bisect
import json
import struct
import sys
from pathlib import Path

from tools.disasm.engine import DisasmEngine
from tools.disasm.loader import DATA_SECTION_NAMES, load_image


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe", type=Path)
    ap.add_argument("--functions", type=Path, required=True)
    ap.add_argument("--seeds", type=Path, required=True)
    ap.add_argument("--analysis-json", type=Path)
    ap.add_argument("--xrefs", type=Path,
                    help="xrefs.json do disasm: imediatos em codigo "
                         "(push/mov offset) tambem contam como ponteiros")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    image = load_image(str(args.xbe),
                       str(args.analysis_json) if args.analysis_json else None)
    engine = DisasmEngine(image)
    code_secs = [s for s in image.sections
                 if s.executable and s.name not in DATA_SECTION_NAMES]
    for s in code_secs:
        engine.linear_sweep(s)
    code_ranges = [(s.virtual_addr, s.virtual_addr + s.virtual_size, s)
                   for s in code_secs]

    def code_section(va):
        for lo, hi, s in code_ranges:
            if lo <= va < hi:
                return s
        return None

    funcs = json.loads(args.functions.read_text())
    as_int = lambda v: int(v, 0) if isinstance(v, str) else v
    bounds = sorted((as_int(f["start"]), as_int(f["end"]), f["name"]) for f in funcs)
    starts = [b[0] for b in bounds]
    start_set = set(starts)

    def containing(va):
        i = bisect.bisect_right(starts, va) - 1
        if i >= 0 and bounds[i][0] < va < bounds[i][1]:
            return bounds[i]
        return None

    def after_boundary(va, sec):
        """O byte anterior fecha uma funcao ou e padding?"""
        data = image.get_section_data(sec)
        off = va - sec.virtual_addr
        if off == 0:
            return True
        if off >= len(data):
            return False
        prev = data[off - 1]
        if prev in (0xCC, 0x90, 0xC3):
            return True
        if off >= 3 and data[off - 3] == 0xC2:          # ret imm16
            return True
        if off >= 5 and data[off - 5] == 0xE9:          # jmp rel32
            return True
        if off >= 2 and data[off - 2] == 0xEB:          # jmp rel8
            return True
        # padding MSVC multi-byte: lea esp,[esp] / mov edi,edi / lea ecx,[ecx]
        if off >= 2 and data[off - 2:off] in (b"\x8b\xff", b"\x8b\xc0"):
            return True
        if off >= 3 and data[off - 3:off] in (b"\x8d\x49\x00", b"\x8d\x40\x00"):
            return True
        if off >= 7 and data[off - 7:off] in (b"\x8d\xa4\x24\x00\x00\x00\x00",
                                              b"\x8d\x80\x00\x00\x00\x00\x90"):
            return True
        if off >= 6 and data[off - 6:off] in (b"\x8d\x9b\x00\x00\x00\x00",
                                              b"\x8d\x80\x00\x00\x00\x00"):
            return True
        return False

    # candidato -> lista de (secao, VA do ponteiro)
    cands = {}
    # Alvos cuja referencia vem de dados ou de um imediato em codigo: ai um
    # alvo "dentro" de uma funcao conhecida e uma funcao engolida (padding de
    # nops, ou chamada sem retorno + int3), nao um case de switch -- as jump
    # tables do MSVC ficam em .text, coladas na funcao.
    strong = set()
    # referenciado por dword em secao de dados cujo vizinho na tabela e um
    # inicio de funcao conhecido (contexto de vtable / tabela de callbacks).
    # Sem essa exigencia, constantes "redondas" (0x00020000, 0x00021000) que
    # por acaso caem em .text viram sementes e partem funcoes reais.
    from_data = set()
    lo_all = min(r[0] for r in code_ranges)
    hi_all = max(r[1] for r in code_ranges)
    for s in image.sections:
        data = image.get_section_data(s)
        if not data:
            continue
        is_code = s in code_secs
        n = len(data) & ~3
        for off in range(0, n, 4):
            (v,) = struct.unpack_from("<I", data, off)
            if v < lo_all or v >= hi_all or v in start_set:
                continue
            ref_va = s.virtual_addr + off
            # dentro de codigo, um dword alinhado so conta se nao estiver no
            # meio de uma funcao conhecida (ali sao instrucoes ou jump tables)
            if is_code and (ref_va in start_set or containing(ref_va)):
                continue
            cands.setdefault(v, []).append((s.name, ref_va))
            if not is_code:
                strong.add(v)
                for noff in (off - 4, off + 4):
                    if 0 <= noff <= n - 4 and \
                            struct.unpack_from("<I", data, noff)[0] in start_set:
                        from_data.add(v)
                        break

    if args.xrefs:
        for x in json.loads(args.xrefs.read_text()):
            if x.get("type") != "data_imm":
                continue
            v = as_int(x["to"])
            if v < lo_all or v >= hi_all or v in start_set:
                continue
            cands.setdefault(v, []).append(("imm", as_int(x["from"])))
            strong.add(v)

    accepted, rej = [], {"nao-codigo": 0, "sem-fronteira": 0, "dentro": 0,
                         "nao-decodifica": 0}
    for va in sorted(cands):
        sec = code_section(va)
        if sec is None or (va & 0xFFF) == 0:   # constantes redondas nao sao ponteiros
            rej["nao-codigo"] += 1
            continue
        inside = containing(va)
        if inside and va not in strong:
            rej["dentro"] += 1
            continue
        # Funcao logo apos uma jump table / tabela de dados embutida no codigo:
        # nao ha padding antes, mas o MSVC alinha o inicio em 16 e o ponteiro
        # vem de uma tabela em .rdata/.data.
        aligned_data_ref = False  # desativado: gera sementes falsas (ver PORT_NOTES.md)
        if not after_boundary(va, sec) and not aligned_data_ref:
            rej["sem-fronteira"] += 1
            continue
        if not engine.probes_as_function_body(va):
            rej["nao-decodifica"] += 1
            continue
        accepted.append((va, inside, cands[va]))

    existing = json.loads(args.seeds.read_text()) if args.seeds.exists() else []
    have = {e["start"].lower() for e in existing if isinstance(e, dict)}
    added = 0
    by_sec = {}
    for va, inside, refs in accepted:
        key = "0x%08X" % va
        by_sec[code_section(va).name] = by_sec.get(code_section(va).name, 0) + 1
        if key.lower() in have:
            continue
        ref = refs[0]
        existing.append({
            "start": key,
            "observed": False,
            "note": "ponteiro em %s @0x%08X (%d ref%s)%s; decodifica como funcao"
                    % (ref[0], ref[1], len(refs), "s" if len(refs) > 1 else "",
                       "; dentro de " + inside[2] if inside else ""),
        })
        added += 1

    print("candidatos: %d  aceitos: %d  rejeitados: %s" %
          (len(cands), len(accepted), rej))
    print("aceitos por secao:", by_sec)
    if args.dry_run:
        print("dry run: %d seriam adicionados" % added)
        return 0
    args.seeds.parent.mkdir(parents=True, exist_ok=True)
    args.seeds.write_text(json.dumps(existing, indent=1))
    print("seeds: %d (+%d) -> %s" % (len(existing), added, args.seeds))
    return 0


if __name__ == "__main__":
    sys.exit(main())
