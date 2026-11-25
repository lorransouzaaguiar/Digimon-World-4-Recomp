#!/usr/bin/env python3
"""
Roda o jogo N vezes SEM JANELA com um cenario de entrada e classifica cada run.

    scripts/soak.py                                  # 1 run do cenario do lobby
    scripts/soak.py --runs 10                        # soak de boot
    scripts/soak.py --scenario tests/scenarios/title.txt --timeout 60
    scripts/soak.py --golden                         # compara os shots com tests/golden
    scripts/soak.py --window                         # com janela (para assistir)
    scripts/soak.py --gamescope --golden             # janela dentro de um gamescope sem tela
    scripts/soak.py --sw                             # rasterizador em CPU (lento)
    scripts/soak.py --opengl --golden                # renderizador OpenGL (o padrao e Vulkan)
    scripts/soak.py -e RECOMP_GUEST_LOCK=1           # variavel extra no ambiente

Cada run tem log, capturas e pasta de saves proprios em
build/soak/<data-hora>/NN/ (build/soak/latest aponta para a ultima invocacao;
nada e apagado). Os saves de verdade em save_data/ nunca sao tocados. Veredito por run, pela
triagem do log (scripts/triage.py):

    OK / AVISOS   o roteiro chegou ao "quit"
    CRASH         [CRASH] no log, ou o processo morreu por sinal
    STALL         [STALL] sem retomar, ou estourou o tempo
    INCOMPLETO    saiu sem chegar ao "quit"

Sem janela: RECOMP_HEADLESS=1 (o runtime nao abre janela nem audio); o back
end OpenGL desenha num contexto EGL offscreen, entao a GPU e usada do mesmo
jeito. Mesma ideia de soak.py (BLiNX 2) e race_loop.sh (SSX 3), feito aqui.
"""
import argparse
import os
import resource
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import triage  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenario", default="tests/scenarios/lobby.txt")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--timeout", type=int, default=240, help="segundos por run")
    ap.add_argument("--window", action="store_true", help="abre a janela do jogo")
    ap.add_argument("--gamescope", metavar="LxA", nargs="?", const="1280x720",
                    help="com janela, mas dentro de um gamescope sem tela (nada aparece no "
                         "monitor): exercita o swapchain do Vulkan e o caminho da janela")
    ap.add_argument("--sw", action="store_true", help="DW4_SW_RENDER=1")
    ap.add_argument("--vulkan", action="store_true", help="DW4_RENDERER=vulkan (ja e o padrao)")
    ap.add_argument("--opengl", action="store_true", help="DW4_RENDERER=opengl")
    ap.add_argument("--golden", action="store_true", help="compara shots com tests/golden")
    ap.add_argument("--wine", action="store_true",
                    help="roda o build de Windows (build-win/dw4_recomp.exe, de "
                         "scripts/build_win.sh) no Wine, sem tela nenhuma")
    ap.add_argument("--binary", default=None,
                    help="padrao: build/dw4_recomp (build-win/dw4_recomp.exe com --wine)")
    ap.add_argument("-e", "--env", action="append", default=[], metavar="VAR=VALOR")
    ap.add_argument("--verbose", action="store_true", help="imprime a triagem de todo run")
    args = ap.parse_args()

    os.chdir(ROOT)
    if args.wine and args.window:
        ap.error("--wine nao abre janela na tela; use --gamescope (janela num gamescope sem tela)")
    if not args.binary:
        args.binary = "build-win/dw4_recomp.exe" if args.wine else "build/dw4_recomp"
    scenario = os.path.abspath(args.scenario)
    # Um diretorio por invocacao (nada e apagado: um run ruim e evidencia);
    # build/soak/latest aponta para o mais recente.
    top = os.path.join(ROOT, "build", "soak")
    base = os.path.join(top, time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(base)
    link = os.path.join(top, "latest")
    if os.path.islink(link):
        os.remove(link)
    if not os.path.exists(link):
        os.symlink(os.path.basename(base), link)
    results = []

    for i in range(1, args.runs + 1):
        d = os.path.join(base, f"{i:02d}")
        os.makedirs(os.path.join(d, "shots"))
        log = os.path.join(d, "run.log")
        env = dict(os.environ)
        env.update({
            "RECOMP_KEYBOARD": "1",
            "DW4_INPUT_SCRIPT": "@" + scenario,
            "DW4_SHOT_DIR": os.path.join(d, "shots"),
            "RECOMP_SAVE_DIR": os.path.join(d, "save"),
            "DW4_GL_STATS": "1",
        })
        cmd = [os.path.join(ROOT, args.binary), os.path.join(ROOT, "game_data")]
        if args.wine:
            # Sem DISPLAY/WAYLAND_DISPLAY o Wine nao tem como abrir janela alguma
            # (nem as dele); o Vulkan sem janela nao precisa de tela. Prefixo
            # proprio, para nao tocar no ~/.wine do usuario. Os caminhos Linux
            # do ambiente valem como estao: o diretorio corrente fica em Z:.
            env.pop("DISPLAY", None)
            env.pop("WAYLAND_DISPLAY", None)
            env.setdefault("WINEPREFIX", os.path.join(ROOT, "tools", "win-deps", "wineprefix"))
            env.setdefault("WINEDEBUG", "-all")
            env.setdefault("WINEDLLOVERRIDES", "mscoree,mshtml=")
            cmd = ["wine"] + cmd
        if args.gamescope:
            gw, _, gh = args.gamescope.partition("x")
            # Sem DISPLAY/WAYLAND_DISPLAY o gamescope nao tem onde se aninhar.
            env.pop("DISPLAY", None)
            env.pop("WAYLAND_DISPLAY", None)
            env.setdefault("SDL_AUDIODRIVER", "dummy")
            cmd = ["gamescope", "--backend", "headless", "-W", gw, "-H", gh or "720", "--"] + cmd
        elif not args.window:
            env["RECOMP_HEADLESS"] = "1"
            env.setdefault("SDL_AUDIODRIVER", "dummy")
        if args.sw:
            env["DW4_SW_RENDER"] = "1"
        if args.vulkan:
            env["DW4_RENDERER"] = "vulkan"
        if args.opengl:
            env["DW4_RENDERER"] = "opengl"
        for kv in args.env:
            k, _, v = kv.partition("=")
            env[k] = v

        t0 = time.time()
        ru0 = resource.getrusage(resource.RUSAGE_CHILDREN)
        timed_out = False
        with open(log, "wb") as f:
            # cwd proprio: o runtime grava xbox_kernel.log no diretorio corrente
            p = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=d,
                                 start_new_session=bool(args.gamescope))
            try:
                rc = p.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                subprocess.run([os.path.join(ROOT, "scripts", "dump_threads.sh")],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                time.sleep(1.0)
                if args.gamescope:
                    os.killpg(p.pid, 9)
                else:
                    p.kill()
                rc = p.wait()
        if args.wine:
            # O servidor do Wine sobrevive alguns segundos ao jogo e seria
            # reaproveitado pelo proximo run com a tela (ou a falta dela) deste.
            subprocess.run(["wineserver", "-k"], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
            subprocess.run(["wineserver", "-w"], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
        secs = time.time() - t0
        ru1 = resource.getrusage(resource.RUSAGE_CHILDREN)
        cpu = (ru1.ru_utime + ru1.ru_stime) - (ru0.ru_utime + ru0.ru_stime)

        verdict, out = triage.triage(log)
        if timed_out and verdict not in ("CRASH", "STALL"):
            verdict = "STALL"
            out.insert(0, f"(estourou o tempo de {args.timeout} s)")
        elif args.gamescope and verdict in ("OK", "AVISOS"):
            pass        # o codigo de saida e do gamescope (ele cai ao fechar), nao do jogo
        elif rc < 0 and verdict != "CRASH":
            verdict = "CRASH"
            out.insert(0, f"(morreu pelo sinal {-rc})")
        elif rc != 0 and verdict in ("OK", "AVISOS"):
            verdict = "INCOMPLETO"
            out.insert(0, f"(saiu com codigo {rc})")
        # A API pedida nao subiu e o jogo caiu na outra: o run nao testou o que devia.
        want = "OpenGL" if args.opengl else "Vulkan" if args.vulkan else None
        if want and verdict in ("OK", "AVISOS"):
            with open(log, "rb") as f:
                active = [ln for ln in f.read().split(b"\n") if b"renderizador ativo:" in ln]
            if not active or want.encode() not in active[0]:
                verdict = "INCOMPLETO"
                out.insert(0, f"(pediu {want}, subiu: "
                              f"{active[0].decode(errors='replace').strip() if active else 'nenhum'})")
        results.append((i, verdict, secs, log))
        print(f"run {i:02d}: {verdict:10s} {secs:6.1f} s  CPU {cpu:6.1f} s ({100 * cpu / secs:3.0f}% de um nucleo)"
              f"  {os.path.relpath(log, ROOT)}", flush=True)
        if args.verbose or verdict not in ("OK", "AVISOS"):
            print("    " + "\n    ".join(out))

    counts = {}
    for _, v, _, _ in results:
        counts[v] = counts.get(v, 0) + 1
    print("resumo: " + ", ".join(f"{n} {v}" for v, n in sorted(counts.items())))

    bad = any(v not in ("OK", "AVISOS") for _, v, _, _ in results)
    if args.golden:
        shots = os.path.join(base, "01", "shots")
        rc = subprocess.call([sys.executable, os.path.join(ROOT, "scripts", "golden.py"), shots])
        bad = bad or rc != 0
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
