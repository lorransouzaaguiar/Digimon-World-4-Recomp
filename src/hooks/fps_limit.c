/**
 * Limitador de quadros: DW4_FPS_LIMIT=30 segura cada flip ate completar
 * 1/30 s desde o anterior. 60 (ou nada) = sem limite extra: o flip ja e
 * preso ao vblank de 60 Hz pelo RECOMP_FLIP_PACING.
 *
 * Chamado na thread do executor a cada flip (gl_host.c). O jogo espera pela
 * GPU, entao segurar o flip segura o jogo inteiro.
 *
 * A logica do Digimon World 4 anda por QUADRO, nao por tempo. Medido em
 * 2026-11-24 com `soak.py --vulkan --golden -e DW4_FPS_LIMIT=30`: os mesmos
 * flips mostram as mesmas telas (os 4 frames de referencia conferem) e o
 * cenario leva 171 s em vez de 92 s. Ou seja, a 60 fps tudo anda duas vezes
 * mais rapido que a 30.
 *
 * Qual e a velocidade "certa": o usuario, que conhece o jogo no console, diz
 * que no Xbox ele rodava a 30 -- por isso o launcher e o run.sh usam 30 como
 * padrao. O que o binario mostra e so metade da historia: o Swap do jogo pede
 * intervalo ONE (um flip por vblank; contador iv1 do "[GPU] flip hold"), mas
 * com vsync um quadro que nao fica pronto a tempo espera o vblank seguinte, e
 * um jogo pesado para o console passa a maior parte do tempo a 30. Nao foi
 * medido em hardware real.
 *
 * O binario sem a variavel roda sem limite extra (60): e assim que os testes
 * sem janela (scripts/soak.py) rodam, para nao dobrar o tempo deles.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xbox_memory_layout.h"
#include "dw4_hooks.h"

void dw4_fps_limit_tick(void)
{
    static long interval_ns = -1;
    static struct timespec next;
    struct timespec now;

    if (interval_ns < 0) {
        const char *e = getenv("DW4_FPS_LIMIT");
        int fps = e ? atoi(e) : 0;

        interval_ns = (fps >= 10 && fps < 60) ? 1000000000L / fps : 0;
        if (interval_ns)
            fprintf(stderr, "[FPS] limite de %d quadros por segundo (DW4_FPS_LIMIT)\n", fps);
    }
    if (!interval_ns)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (next.tv_sec && (now.tv_sec < next.tv_sec
            || (now.tv_sec == next.tv_sec && now.tv_nsec < next.tv_nsec))) {
        /* Enquanto esta thread dorme ninguem responde ao kick do D3D: o
         * jogo espera dormindo tambem (xbox_Nv2aKickGate), nao girando. */
        xbox_Nv2aHostStall(1);
        for (;;) {
            /* Em fatias de 0,5 ms: um kick que passou pelo portao logo antes
             * (RECOMP_KICK_AHEAD) e respondido daqui, e o jogo nao fica
             * girando a espera durante o sono todo. */
            struct timespec slice = now;

            slice.tv_nsec += 500000L;
            if (slice.tv_nsec >= 1000000000L) {
                slice.tv_nsec -= 1000000000L;
                slice.tv_sec++;
            }
            if (slice.tv_sec > next.tv_sec
                    || (slice.tv_sec == next.tv_sec && slice.tv_nsec >= next.tv_nsec))
                slice = next;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &slice, NULL);
            xbox_Nv2aAckGranted();
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > next.tv_sec
                    || (now.tv_sec == next.tv_sec && now.tv_nsec >= next.tv_nsec))
                break;
        }
        xbox_Nv2aHostStall(0);
    } else {
        next = now;                     /* atrasado: recomeca daqui, sem compensar */
    }
    next.tv_nsec += interval_ns;
    if (next.tv_nsec >= 1000000000L) {
        next.tv_nsec -= 1000000000L;
        next.tv_sec++;
    }
}
