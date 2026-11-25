/**
 * host_os.h para Linux: /proc, sinais POSIX, backtrace da glibc.
 *
 * macOS ainda nao foi portado: dw4_os_exe_dir (/proc/self/exe) e
 * dw4_dump_all_threads (/proc/self/task, tgkill) sao so do Linux, e os
 * registradores em ucontext tem outros nomes.
 */
#if !defined(_WIN32)

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "kernel.h"
#include "xbox_memory_layout.h"

#include "dw4_hooks.h"
#include "host_os.h"

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Caminhos e ambiente ───────────────────────────────────────────────────── */

int dw4_os_exe_dir(char *out, size_t n)
{
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *slash;

    if (len <= 0)
        return 0;
    exe[len] = '\0';
    slash = strrchr(exe, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    snprintf(out, n, "%s", exe);
    return 1;
}

int dw4_os_abspath(const char *in, char *out, size_t n)
{
    char buf[PATH_MAX];

    if (!realpath(in, buf))
        return 0;
    snprintf(out, n, "%s", buf);
    return 1;
}

void dw4_os_mkdir(const char *path)
{
    mkdir(path, 0755);
}

void dw4_os_env_default(const char *name, const char *value)
{
    setenv(name, value, 0);
}

void dw4_os_env_defaults(void)
{
}

/* ── Crash handler ─────────────────────────────────────────────────────────── */

static void fault_handler(int sig, siginfo_t *si, void *ucv)
{
    uintptr_t fault = (uintptr_t)si->si_addr;
    ucontext_t *uc = (ucontext_t *)ucv;
    static volatile int in_crash;

    /* Registradores de hardware interceptados (APU, AC'97, USB OHCI): o runtime
     * decodifica o acesso, emula e avanca o RIP. 1 = tratado, retoma. */
    if (xbox_PosixMmioFault(ucv, fault))
        return;

    if (__atomic_exchange_n(&in_crash, 1, __ATOMIC_SEQ_CST)) {
        /* Falha dentro do proprio relatorio (ou em outra thread): termina. */
        signal(sig, SIG_DFL);
        return;
    }

    /* fprintf nao e async-signal-safe, mas o processo ja esta condenado e o
     * relatorio e a unica coisa que importa a partir daqui. */
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    fprintf(stderr,
            "\n[CRASH] %s em RIP=0x%lx, endereco=0x%lx (VA guest 0x%08X)\n",
            sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" :
            sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" :
            sig == SIGABRT ? "SIGABRT (abort da libc)" : "sinal",
            (unsigned long)rip, (unsigned long)fault,
            (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
    dw4_crash_report_regs();

    /* Funcao do guest: cada sub_XXXXXXXX e um simbolo real (-rdynamic). */
    {
        void *pc[1] = { (void *)rip };
        fprintf(stderr, "  em: ");
        fflush(stderr);
        backtrace_symbols_fd(pc, 1, STDERR_FILENO);
    }

    dw4_crash_report_guest_stack();
    fprintf(stderr, "  backtrace do host:\n");
    fflush(stderr);
    {
        void *bt[48];
        int n = backtrace(bt, 48);
        backtrace_symbols_fd(bt, n, STDERR_FILENO);
    }
    dw4_hooks_crash_report();
    fflush(stderr);

    /* Devolve ao handler padrao: a instrucao refaz a falha e gera o core. */
    signal(sig, SIG_DFL);
}

/* SIGUSR1: despeja o estado DESTA thread sem derrubar nada. Com
 * scripts/dump_threads.sh (tgkill em cada thread) da para ver onde um jogo
 * travado esta, mesmo com ptrace bloqueado (yama ptrace_scope=1). */
static void dump_handler(int sig, siginfo_t *si, void *ucv)
{
    static volatile int lock;
    ucontext_t *uc = (ucontext_t *)ucv;
    void *pc[1] = { (void *)uc->uc_mcontext.gregs[REG_RIP] };
    void *bt[40];
    int n;

    (void)sig; (void)si;
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE))
        ;
    fprintf(stderr, "\n[DUMP] thread %d  guest esp=%08X eax=%08X ecx=%08X "
                    "esi=%08X edi=%08X ebp=%08X\n  em: ",
            (int)gettid(), g_esp, g_eax, g_ecx, g_esi, g_edi, g_ebp);
    fflush(stderr);                 /* stderr tem buffer; o backtrace vai direto ao fd */
    backtrace_symbols_fd(pc, 1, STDERR_FILENO);
    if (g_xbox_mem_offset && g_esp >= 0x10000) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0;
        fprintf(stderr, "  retornos do guest:");
        for (int i = 0; i < 400 && shown < 20; i++)
            if (sp[i] > g_xbox_code_lo && sp[i] < g_xbox_code_hi) {
                fprintf(stderr, " %08X", sp[i]);
                shown++;
            }
        fprintf(stderr, "\n");
    }
    fflush(stderr);                 /* antes das escritas diretas no fd */
    n = backtrace(bt, 40);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

void dw4_os_install_crash_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    /* abort() da glibc ("malloc(): unaligned tcache chunk detected" etc.):
     * sem isto o processo morria sem dizer em que funcao do guest estava. */
    sigaction(SIGABRT, &sa, NULL);

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = dump_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

/* Pede a cada thread do processo (menos esta) o despejo do dump_handler. */
void dw4_dump_all_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    pid_t pid = getpid(), self = (pid_t)syscall(SYS_gettid);

    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        pid_t tid = (pid_t)atoi(e->d_name);
        struct timespec ts = { 0, 20000000 };      /* 20 ms: saidas nao se misturam */

        if (tid <= 0 || tid == self)
            continue;
        syscall(SYS_tgkill, pid, tid, SIGUSR1);
        nanosleep(&ts, NULL);
    }
    closedir(d);
}

/* ── Mostrador de desempenho (osd.c) ───────────────────────────────────────── */

int dw4_os_cpu_times(double *busy, double *total)
{
    /* /proc/stat, linha "cpu": user nice system idle iowait irq softirq steal */
    unsigned long long v[8] = { 0 };
    FILE *f = fopen("/proc/stat", "r");
    int n;

    if (!f)
        return 0;
    n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    fclose(f);
    if (n < 4)
        return 0;
    *busy = (double)(v[0] + v[1] + v[2] + v[5] + v[6] + v[7]);
    *total = *busy + (double)(v[3] + v[4]);
    return 1;
}

/* NVIDIA: a biblioteca de gerencia do driver (NVML), carregada so se existir. */
static int gpu_nvml(void)
{
    static int state;                   /* 0 nao tentou, 1 pronto, -1 nao ha */
    static void *dev;
    static int (*get_util)(void *, unsigned *);
    unsigned util[2] = { 0, 0 };        /* nvmlUtilization_t: gpu, memory */

    if (!state) {
        void *lib = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        int (*init)(void) = lib ? (int (*)(void))dlsym(lib, "nvmlInit_v2") : NULL;
        int (*by_index)(unsigned, void **) =
            lib ? (int (*)(unsigned, void **))dlsym(lib, "nvmlDeviceGetHandleByIndex_v2") : NULL;

        get_util = lib ? (int (*)(void *, unsigned *))dlsym(lib, "nvmlDeviceGetUtilizationRates") : NULL;
        state = init && by_index && get_util && init() == 0 && by_index(0, &dev) == 0 ? 1 : -1;
    }
    if (state < 0 || get_util(dev, util) != 0)
        return -1;
    return (int)util[0];
}

/* AMD (amdgpu): /sys/class/drm/cardN/device/gpu_busy_percent. */
static int gpu_sysfs(void)
{
    static char path[64];
    static int state;
    FILE *f;
    int v = -1;

    for (int i = 0; !state && i < 8; i++) {
        snprintf(path, sizeof path, "/sys/class/drm/card%d/device/gpu_busy_percent", i);
        if (access(path, R_OK) == 0)
            state = 1;
    }
    if (state != 1) {
        state = -1;
        return -1;
    }
    f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%d", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

int dw4_os_gpu_percent(void)
{
    int v = gpu_nvml();

    return v >= 0 ? v : gpu_sysfs();
}

#endif /* !_WIN32 */
