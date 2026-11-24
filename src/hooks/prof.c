/**
 * Amostrador de CPU embutido (ferramenta de desenvolvimento, so Linux).
 *
 *   DW4_PROF=<arquivo>   liga; o arquivo e escrito ao sair (quit do roteiro,
 *                        exit normal) e lido por scripts/prof_report.py
 *   DW4_PROF_HZ=<n>      amostras por segundo de CPU (padrao 1000)
 *   DW4_PROF_FROM=<n>    so amostra a partir do flip n (pula boot e menus)
 *
 * Existe porque a maquina de desenvolvimento nao tem `perf`. ITIMER_PROF conta
 * tempo de CPU do processo inteiro e o kernel entrega o SIGPROF a uma thread
 * que esta rodando, entao a distribuicao das amostras por thread e por funcao
 * acompanha o consumo de CPU. Cada amostra guarda a thread, o RIP e o endereco
 * de retorno no topo da pilha (bom palpite de "quem chamou" em funcao folha).
 *
 * O arquivo traz /proc/self/maps e o nome de cada thread; a traducao para
 * simbolos e feita fora do processo (nm), sem custo durante o jogo.
 */
#if defined(__linux__) && defined(__x86_64__)

#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include "dw4_hooks.h"

typedef struct {
    uint64_t rip, ret;
    int32_t tid;
} Sample;

#define MAX_SAMPLES (4u * 1024u * 1024u)

static Sample *s_samples;
static uint32_t s_count;                /* atomico */
static const char *s_path;
static uint64_t s_from_flip;
static int s_armed;

static void prof_handler(int sig, siginfo_t *si, void *ucv)
{
    const ucontext_t *uc = (const ucontext_t *)ucv;
    uint32_t i;

    (void)sig; (void)si;
    if (!s_armed) {
        if (dw4_frame_count() < s_from_flip)
            return;
        s_armed = 1;
    }
    i = __atomic_fetch_add(&s_count, 1, __ATOMIC_RELAXED);
    if (i >= MAX_SAMPLES)
        return;
    s_samples[i].rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    s_samples[i].tid = (int32_t)gettid();
    {
        /* [rsp] so e o retorno logo na entrada de uma funcao folha; o
         * relatorio so o usa quando cai dentro de codigo mapeado. */
        const uint64_t *sp = (const uint64_t *)uc->uc_mcontext.gregs[REG_RSP];
        s_samples[i].ret = sp ? *sp : 0;
    }
}

void dw4_prof_dump(void)
{
    static int done;
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    uint32_t n;
    FILE *out, *in;
    char line[512];

    if (!s_samples || done)
        return;
    done = 1;
    setitimer(ITIMER_PROF, &off, NULL);
    n = __atomic_load_n(&s_count, __ATOMIC_RELAXED);
    if (n > MAX_SAMPLES)
        n = MAX_SAMPLES;
    out = fopen(s_path, "w");
    if (!out)
        return;
    in = fopen("/proc/self/maps", "r");
    if (in) {
        while (fgets(line, sizeof line, in))
            if (strstr(line, " r-xp "))
                fprintf(out, "M %s", line);
        fclose(in);
    }
    {
        int32_t seen[256];
        int nseen = 0;

        for (uint32_t i = 0; i < n; i++) {
            int32_t tid = s_samples[i].tid;
            int k = 0;
            char path[64], comm[64] = "?";

            while (k < nseen && seen[k] != tid)
                k++;
            if (k < nseen || nseen == 256)
                continue;
            seen[nseen++] = tid;
            snprintf(path, sizeof path, "/proc/self/task/%d/comm", tid);
            in = fopen(path, "r");
            if (in) {
                if (fgets(comm, sizeof comm, in))
                    comm[strcspn(comm, "\n")] = '\0';
                fclose(in);
            }
            fprintf(out, "T %d %s\n", tid, comm);
        }
    }
    fprintf(out, "F %llu\n", (unsigned long long)dw4_frame_count());
    for (uint32_t i = 0; i < n; i++)
        fprintf(out, "S %d %llx %llx\n", s_samples[i].tid,
                (unsigned long long)s_samples[i].rip, (unsigned long long)s_samples[i].ret);
    fclose(out);
    fprintf(stderr, "[PROF] %u amostras em %s\n", n, s_path);
}

void dw4_prof_init(void)
{
    const char *e = getenv("DW4_PROF");
    struct sigaction sa;
    struct itimerval it;
    int hz = 1000;

    if (!e || !*e)
        return;
    s_path = e;
    s_samples = calloc(MAX_SAMPLES, sizeof *s_samples);
    if (!s_samples)
        return;
    if (getenv("DW4_PROF_HZ"))
        hz = atoi(getenv("DW4_PROF_HZ"));
    if (hz < 10 || hz > 10000)
        hz = 1000;
    if (getenv("DW4_PROF_FROM"))
        s_from_flip = strtoull(getenv("DW4_PROF_FROM"), NULL, 10);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 1000000 / hz;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
    atexit(dw4_prof_dump);
    fprintf(stderr, "[PROF] amostrando a %d Hz de CPU a partir do flip %llu -> %s\n", hz,
            (unsigned long long)s_from_flip, s_path);
}

#else

void dw4_prof_init(void) {}
void dw4_prof_dump(void) {}

#endif
