/**
 * Digimon World 4 (Xbox, NTSC-U) - host nativo (Linux; Windows em andamento)
 *
 * XBE:
 *   Titulo:        DigimonWorld4  (Title ID 0x42410002, engine Alchemy)
 *   Base:          0x00010000     Entry point: 0x000121A4
 *   XDK:           1.0.5849 (D3D8, D3DX8, DSOUND, XAPILIB, LIBCMT linkados
 *                  estaticamente) + CRI Sofdec (secoes PSFD_*)
 *   Kernel thunks: 0x00309BC0 (128 imports)
 *
 * Sequencia de boot (mesma do templates/new-game do xboxrecomp):
 *   1. falhas de acesso -> MMIO emulado (APU/AC97/OHCI) ou crash report
 *      (sinais no POSIX, excecoes no Windows: hooks/host_os_*.c)
 *   2. carrega o XBE de ./game_data (busca case-insensitive: FATX != ext4/btrfs)
 *   3. xbox_MemoryLayoutInit  -> 64 MB de RAM do guest + mirrors
 *   4. xbox_kernel_init / xbox_path_init (D:\ -> ./game_data) / bridge
 *   5. g_esp = topo da pilha do guest, tabela de dispatch
 *   6. xbe_entry_point()  (a funcao recompilada em 0x000121A4)
 *
 * A janela SDL precisa da thread principal: xbox_HostWindowMain() roda o
 * guest numa pthread e fica na principal bombeando eventos/present.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "video_player.h"     /* xbox_HostWindowMain */

#include "hooks/dw4_hooks.h"
#include "hooks/host_os.h"

#define DW4_ENTRY_POINT 0x000121A4u

/* ── Registradores do guest (definidos em xbox_memory_layout.c) ──────────────
 * RECOMP_TLS e obrigatorio: sao thread-local no runtime. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

/* Gerados pelo recompilador */
extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);

/* APU emulada (declarado aqui para nao depender do include path de src/apu) */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;

static char s_game_dir[PATH_MAX];
static char s_save_dir[PATH_MAX];
static char s_xbe_path[PATH_MAX];

/* ── Relatorio de crash: a parte que nao depende do sistema ─────────────────
 * Quem captura a falha (sinal ou excecao) e o backtrace do host ficam em
 * hooks/host_os_*.c. O formato das linhas e lido por scripts/triage.py. */

void dw4_crash_report_regs(void)
{
    fprintf(stderr, "  guest: eax=%08X ecx=%08X edx=%08X ebx=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "         esi=%08X edi=%08X ebp=%08X esp=%08X\n",
            g_esi, g_edi, g_ebp, g_esp);
}

void dw4_crash_report_guest_stack(void)
{
    /* Cadeia de chamadas do guest: toda chamada recompilada empilha o
     * endereco de retorno do guest, entao a pilha do guest ainda a contem. */
    if (g_xbox_mem_offset && g_esp >= 0x10000 &&
        (size_t)g_esp + 1024 < xbox_GetMappedSize()) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0;
        fprintf(stderr, "  pilha do guest (enderecos de retorno):\n");
        for (int i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        if (!shown) {
            fprintf(stderr, "    (nenhum endereco de retorno no intervalo)\n");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, "    raw[esp+%-3d] 0x%08X\n", i * 4, sp[i]);
        }
    }
    fprintf(stderr, "  ICALLs recentes:");
    for (unsigned k = 0; k < 16; k++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
    fprintf(stderr, "\n");
}

/* ── Arquivos ──────────────────────────────────────────────────────────────── */

static bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Procura `name` em `dir` ignorando caixa (o disco tem "Default.xbe"). */
static bool find_nocase(const char *dir, const char *name, char *out, size_t n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    bool ok = false;

    if (!d)
        return false;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, n, "%s/%s", dir, e->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

/* Diretorio do jogo: argv[1], $DW4_GAME_DIR, ./game_data, ou o game_data ao
 * lado do executavel / na raiz do projeto. Sempre resolvido para absoluto. */
static bool resolve_game_dir(int argc, char **argv)
{
    char exe[PATH_MAX] = "", cand[PATH_MAX + 16], cand2[PATH_MAX + 16];
    const char *tries[6];
    int n = 0;

    if (argc > 1 && argv[1][0] != '-')
        tries[n++] = argv[1];
    if (getenv("DW4_GAME_DIR"))
        tries[n++] = getenv("DW4_GAME_DIR");
    tries[n++] = "game_data";
    if (dw4_os_exe_dir(exe, sizeof(exe))) {
        /* Pacote de release: game_data/ ao lado do executavel. */
        snprintf(cand2, sizeof(cand2), "%s/game_data", exe);
        tries[n++] = cand2;
        snprintf(cand, sizeof(cand), "%s/../game_data", exe);
        tries[n++] = cand;
    }
#ifdef DW4_PROJECT_ROOT          /* so no build de desenvolvimento */
    tries[n++] = DW4_PROJECT_ROOT "/game_data";
#endif

    for (int i = 0; i < n; i++) {
        if (is_dir(tries[i]) && dw4_os_abspath(tries[i], s_game_dir, sizeof(s_game_dir)))
            return true;
    }
    return false;
}

static void *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    rewind(f);
    if (size <= 0 || !(data = malloc((size_t)size))) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return data;
}

/* ── Boot do guest ─────────────────────────────────────────────────────────── */

static int guest_main(void)
{
    size_t xbe_size = 0;
    void *xbe_data = load_file(s_xbe_path, &xbe_size);

    if (!xbe_data) {
        fprintf(stderr, "[BOOT] nao foi possivel ler %s\n", s_xbe_path);
        return 1;
    }
    printf("[BOOT] XBE: %s (%zu bytes)\n", s_xbe_path, xbe_size);

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fprintf(stderr, "[BOOT] xbox_MemoryLayoutInit falhou\n");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("[BOOT] memoria do guest mapeada, offset=0x%llx\n",
           (unsigned long long)g_xbox_mem_offset);

    /* APU emulada: so com RECOMP_AC97_READY (que tambem desmapeia os
     * registradores para serem interceptados) -- as duas metades andam juntas. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] APU emulada %s\n",
                g_apu_state ? "ativa" : "FALHOU ao inicializar");
    }

    /* Controladores USB (OHCI) da MCPX: o XAPI do jogo enumera o controle
     * direto nesses registradores. Nada no toolkit chama isto sozinho; sem a
     * chamada RECOMP_USB e inerte e o jogo nao ve controle algum. Precisa da
     * memoria do guest ja mapeada. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    xbox_kernel_init();
    xbox_path_init(s_game_dir, s_save_dir);
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] dispatch plano indisponivel; ICALLs usarao "
                        "busca binaria\n");

    dw4_hooks_init();
    xbox_WatchdogStart();

    printf("[BOOT] entry point 0x%08X, esp=0x%08X -- iniciando o jogo\n",
           DW4_ENTRY_POINT, g_esp);
    fflush(stdout);

    xbe_entry_point();

    printf("[BOOT] o jogo retornou do entry point\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}

static void *log_flush_thread(void *arg)
{
    struct timespec ts = { 0, 250000000 };

    (void)arg;
    for (;;) {
        nanosleep(&ts, NULL);
        fflush(stdout);
        fflush(stderr);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    /* Log com buffer de 64 KB e descarga a cada 250 ms por uma thread. Sem
     * buffer, cada fprintf e uma syscall sob o lock do stderr e serializa
     * todas as threads, o executor NV2A inclusive (licao do port do Def Jam).
     * O handler de crash e o de despejo descarregam por conta propria.
     * DW4_LOG_UNBUFFERED=1 volta ao modo antigo. */
    if (getenv("DW4_LOG_UNBUFFERED")) {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    } else {
        static char out_buf[65536], err_buf[65536];
        pthread_t t;
        setvbuf(stdout, out_buf, _IOFBF, sizeof out_buf);
        setvbuf(stderr, err_buf, _IOFBF, sizeof err_buf);
        if (pthread_create(&t, NULL, log_flush_thread, NULL) == 0)
            pthread_detach(t);
    }

    printf("=== Digimon World 4 - recompilacao estatica (port nativo) ===\n");

    if (!resolve_game_dir(argc, argv)) {
        fprintf(stderr, "[BOOT] diretorio do jogo nao encontrado. Use "
                        "./dw4_recomp <dir> ou DW4_GAME_DIR=<dir>\n");
        return 1;
    }
    if (!find_nocase(s_game_dir, "default.xbe", s_xbe_path, sizeof(s_xbe_path))) {
        fprintf(stderr, "[BOOT] default.xbe nao encontrado em %s\n", s_game_dir);
        return 1;
    }

    /* Saves/cache (E:\UDATA, E:\TDATA, Z:\) ficam dentro do projeto. */
    if (getenv("RECOMP_SAVE_DIR")) {
        snprintf(s_save_dir, sizeof(s_save_dir), "%s", getenv("RECOMP_SAVE_DIR"));
    } else {
        char parent[PATH_MAX + 8];
#ifndef DW4_PROJECT_ROOT
        /* Pacote de release: saves ao lado do executavel, mesmo que game_data
         * seja um link para outro lugar. */
        char exe[PATH_MAX] = "";
        if (dw4_os_exe_dir(exe, sizeof(exe))) {
            snprintf(parent, sizeof(parent), "%s", exe);
        } else
#endif
        snprintf(parent, sizeof(parent), "%s/..", s_game_dir);
        if (!dw4_os_abspath(parent, s_save_dir, sizeof(s_save_dir)))
            snprintf(s_save_dir, sizeof(s_save_dir), "%s", s_game_dir);
        strncat(s_save_dir, "/save_data",
                sizeof(s_save_dir) - strlen(s_save_dir) - 1);
    }
    dw4_os_mkdir(s_save_dir);

    printf("[BOOT] D:\\ -> %s\n[BOOT] saves -> %s\n", s_game_dir, s_save_dir);

    dw4_hooks_set_env_defaults();
    dw4_os_install_crash_handlers();

    return xbox_HostWindowMain(guest_main);
}
