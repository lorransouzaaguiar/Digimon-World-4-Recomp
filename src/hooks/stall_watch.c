/**
 * Amostrador de travamento.
 *
 * Se nenhum frame for apresentado por DW4_STALL_MS milissegundos (padrao
 * 5000; 0 desliga), escreve "[STALL] ..." no log e pede a TODAS as threads
 * que despejem seu estado (dw4_dump_all_threads em host_os_*.c: registradores e
 * retornos do guest, backtrace do host). Assim um travamento visto por quem
 * esta jogando ja chega diagnosticado no log, sem precisar reproduzir.
 *
 * So arma depois do primeiro flip (o boot fica segundos sem apresentar nada)
 * e dispara uma vez por episodio; quando os flips voltam, registra
 * "[STALL] retomou" e arma de novo.
 *
 * Mesma ideia de AMPED2_STALL_MS (bertibertig/Amped2-Recomp) e
 * RECOMP_SLOW_STACKS (koenhendriks/SSX3-recomp); implementacao propria.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "dw4_hooks.h"

static unsigned s_stall_ms = 5000;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void *stall_thread(void *arg)
{
    uint64_t last = 0;
    double last_change = now_s();
    int fired = 0;

    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 100000000 };     /* 100 ms */
        uint64_t n;
        double now;

        nanosleep(&ts, NULL);
        n = dw4_frame_count();
        now = now_s();
        if (n != last) {
            if (fired) {
                fprintf(stderr, "[STALL] retomou apos %.1f s sem flip (flip %llu)\n",
                        now - last_change, (unsigned long long)n);
                fired = 0;
            }
            last = n;
            last_change = now;
            continue;
        }
        if (!n || fired || (now - last_change) * 1000.0 < (double)s_stall_ms)
            continue;
        fired = 1;
        fprintf(stderr, "[STALL] nenhum flip ha %.1f s (ultimo flip %llu); "
                        "despejando todas as threads\n",
                now - last_change, (unsigned long long)n);
        fflush(stderr);
        dw4_dump_all_threads();
        fprintf(stderr, "[STALL] fim do despejo\n");
        fflush(stderr);
    }
    return NULL;
}

void dw4_stall_watch_init(void)
{
    const char *e = getenv("DW4_STALL_MS");
    pthread_t t;

    if (e)
        s_stall_ms = (unsigned)atoi(e);
    if (!s_stall_ms)
        return;
    if (pthread_create(&t, NULL, stall_thread, NULL) == 0)
        pthread_detach(t);
}
