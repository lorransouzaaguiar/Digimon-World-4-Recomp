#!/usr/bin/env python3
"""
Resume um log do dw4_recomp numa tela e da um veredito.

    scripts/triage.py build/run.log
    scripts/triage.py build/soak/run-01.log --quiet     # so o veredito

Vereditos, do mais grave para o menos:
    CRASH      houve [CRASH] (sinal fatal ou abort da libc)
    STALL      houve [STALL] sem "[STALL] retomou" depois
    INCOMPLETO o roteiro nao chegou ao "quit" e nada acima explica (NAO e "ok")
    AVISOS     terminou, mas com alvos de ICALL sem resolucao, instrucoes nao
               traduzidas alcancadas, passos do roteiro sem efeito etc.
    OK

Regras de leitura (do port do Def Jam): um acerto e evidencia, uma ausencia
nao; leia totais, nao linhas impressas.
"""
import collections
import re
import sys

NOISE = re.compile(r"^\s*\[(KERNEL|HEAP|CS|CONTIG|READ|GPU|PB|NV2A|APU|PATH|TEXUSE|OHCI\d?|BRIDGE|XBE)\]|^\s+GS |token=|^\[GPU\]|^\[PB\]")


def triage(path):
    try:
        lines = open(path, errors="replace").read().splitlines()
    except OSError as e:
        return "SEM-LOG", [str(e)]

    out = []
    crash_at = next((i for i, l in enumerate(lines) if l.startswith("[CRASH]")), None)
    stalls = [i for i, l in enumerate(lines) if l.startswith("[STALL] nenhum flip")]
    resumes = [i for i, l in enumerate(lines) if l.startswith("[STALL] retomou")]
    quit_ok = any(l.startswith("[SCRIPT] quit") for l in lines)
    script_fail = [l for l in lines if l.startswith("[SCRIPT]") and
                   ("SEM EFEITO" in l or "ESGOTADO" in l or "FALHOU" in l or "desconhecid" in l)]

    icall = collections.Counter(re.findall(r"Failed to resolve VA (0x[0-9A-Fa-f]+)", "\n".join(lines)))
    notcode = collections.Counter(re.findall(r"alvo (0x[0-9A-Fa-f]+) nao e codigo", "\n".join(lines)))
    unimpl = collections.Counter(re.findall(r"\[UNIMPL\].*?`([^`]+)` em (0x[0-9A-Fa-f]+)", "\n".join(lines)))
    stubs = collections.Counter(re.findall(r"\[STUB\] unresolved (0x[0-9A-Fa-f]+)", "\n".join(lines)))
    missing = sorted(set(re.findall(r"\[FILE\] missing (\S+)", "\n".join(lines))))
    mmio = [l for l in lines if "decode fail" in l or "undecodable" in l]
    asan = [l for l in lines if "AddressSanitizer" in l or l.startswith("malloc()") or "free():" in l]

    gl = next((l for l in lines if l.startswith("[GL] back end")), None)
    gl_fallback = next((l for l in lines if l.startswith("[GL]") and "rasterizador em CPU" in l), None)
    fps = [float(m.group(1)) for l in lines for m in [re.match(r"\[GL\] ([0-9.]+) fps", l)] if m]
    marks = [l for l in lines if l.startswith("[SCRIPT] mark")]

    out.append(f"log: {path} ({len(lines)} linhas)")
    out.append("render: " + (gl[5:] if gl else (gl_fallback[5:] if gl_fallback else "(sem linha [GL])")))
    if fps:
        out.append(f"fps: min {min(fps):.1f}  media {sum(fps)/len(fps):.1f}  max {max(fps):.1f}  ({len(fps)} amostras de 5 s)")
    if marks:
        out.append("roteiro: " + " -> ".join(m.split()[2] for m in marks) + (" -> quit" if quit_ok else "  (sem quit)"))
    for l in script_fail:
        out.append("  ! " + l)
    if icall:
        out.append(f"ICALL sem resolucao: {sum(icall.values())} ocorrencias, {len(icall)} alvos: "
                   + " ".join(f"{va}x{n}" for va, n in icall.most_common(8)))
        out.append("  -> semear: tools.seed_from_log <log> ... + scripts/regen.sh")
    if notcode:
        out.append(f"ICALL para nao-codigo: {len(notcode)} alvos: " + " ".join(list(notcode)[:8]))
    if unimpl:
        out.append("instrucoes nao traduzidas alcancadas: "
                   + " ".join(f"{m}@{va}" for (m, va), _ in unimpl.most_common(8)))
    if stubs:
        out.append("stubs nao resolvidos alcancados: " + " ".join(list(stubs)[:8]))
    if missing:
        out.append("arquivos ausentes: " + " ".join(missing[:8]))
    if mmio:
        out.append(f"MMIO sem decodificacao: {len(mmio)} (ex.: {mmio[0].strip()[:90]})")
    if asan:
        out.append("memoria: " + asan[0].strip()[:120])

    if crash_at is not None:
        out.append("--- crash ---")
        out.extend(lines[crash_at:crash_at + 22])
        verdict = "CRASH"
    elif stalls and (not resumes or resumes[-1] < stalls[-1]):
        out.append("--- travamento (nao retomou) ---")
        dumps = [l for l in lines[stalls[-1]:] if l.startswith("[DUMP]") or l.startswith("  retornos do guest")]
        out.extend([lines[stalls[-1]]] + dumps[:12])
        verdict = "STALL"
    elif not quit_ok and any(l.startswith("[SCRIPT]") for l in lines):
        verdict = "INCOMPLETO"
    elif icall or stubs or mmio or asan or script_fail:
        verdict = "AVISOS"
    else:
        verdict = "OK"
    if stalls and verdict not in ("STALL", "CRASH"):
        out.append(f"travamentos que retomaram: {len(stalls)}")

    tail = [l for l in lines if not NOISE.search(l)][-8:]
    out.append("--- ultimas linhas ---")
    out.extend(l[:160] for l in tail)
    return verdict, out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    quiet = "--quiet" in sys.argv
    if not args:
        print(__doc__)
        return 2
    worst = 0
    order = ["OK", "AVISOS", "INCOMPLETO", "STALL", "CRASH", "SEM-LOG"]
    for path in args:
        verdict, out = triage(path)
        if quiet:
            print(f"{verdict:10s} {path}")
        else:
            print("\n".join(out))
            print(f"=== VEREDITO: {verdict} ===")
        worst = max(worst, order.index(verdict))
    return 0 if worst <= 1 else 1


if __name__ == "__main__":
    sys.exit(main())
