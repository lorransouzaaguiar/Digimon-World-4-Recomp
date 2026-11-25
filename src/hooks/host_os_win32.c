/**
 * host_os.h para Windows (MinGW-w64).
 *
 * Falhas de acesso chegam por excecao estruturada, nao por sinal:
 *   - um manipulador vetorizado entrega as violacoes de acesso ao toolkit
 *     (xbox_VehMmioFault: registradores de hardware emulados) e retoma;
 *   - o que ninguem tratou cai no filtro de excecao nao tratada, que escreve o
 *     relatorio de crash. Fica no filtro, e nao no manipulador vetorizado,
 *     porque o toolkit tem outros manipuladores proprios e uma falha que um
 *     deles resolve nao e crash.
 *
 * Enderecos do host saem como "dw4_recomp.exe+0xDESLOC": o MinGW gera DWARF, que
 * o dbghelp nao le. Para o nome da funcao (sub_XXXXXXXX = funcao do guest):
 *     x86_64-w64-mingw32-addr2line -f -e dw4_recomp.exe <base da imagem + desloc>
 *
 * A fazer: dw4_dump_all_threads (no Linux e por SIGUSR1; aqui seria suspender
 * cada thread e ler o contexto).
 */
#if defined(_WIN32)

#include <windows.h>
#include <direct.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_memory_layout.h"

#include "dw4_hooks.h"
#include "host_os.h"

extern ptrdiff_t g_xbox_mem_offset;

/* ── Caminhos e ambiente ───────────────────────────────────────────────────── */

int dw4_os_exe_dir(char *out, size_t n)
{
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash;

    if (!len || len >= sizeof exe)
        return 0;
    slash = strrchr(exe, '\\');
    if (!slash)
        return 0;
    *slash = '\0';
    snprintf(out, n, "%s", exe);
    return 1;
}

int dw4_os_abspath(const char *in, char *out, size_t n)
{
    char buf[MAX_PATH];

    if (!_fullpath(buf, in, sizeof buf)
            || GetFileAttributesA(buf) == INVALID_FILE_ATTRIBUTES)
        return 0;
    snprintf(out, n, "%s", buf);
    return 1;
}

void dw4_os_mkdir(const char *path)
{
    _mkdir(path);
}

void dw4_os_env_default(const char *name, const char *value)
{
    if (!getenv(name))
        _putenv_s(name, value);
}

void dw4_os_env_defaults(void)
{
    /* Sem isto, em monitor com escala (125%, 150%...) o Windows estica a
     * janela como bitmap: imagem borrada e resolucao real menor que a pedida.
     * A SDL le as dicas dela tambem do ambiente. */
    dw4_os_env_default("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
}

/* ── Crash handler ─────────────────────────────────────────────────────────── */

static uintptr_t s_image_lo, s_image_hi;

static void print_host_addr(uintptr_t a)
{
    if (a >= s_image_lo && a < s_image_hi)
        fprintf(stderr, "dw4_recomp.exe+0x%llX\n", (unsigned long long)(a - s_image_lo));
    else
        fprintf(stderr, "0x%llX\n", (unsigned long long)a);
}

static const char *exception_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:      return "violacao de acesso";
    case EXCEPTION_ILLEGAL_INSTRUCTION:   return "instrucao ilegal";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "divisao inteira por zero";
    case EXCEPTION_STACK_OVERFLOW:        return "estouro de pilha";
    case EXCEPTION_IN_PAGE_ERROR:         return "erro de pagina";
    case EXCEPTION_PRIV_INSTRUCTION:      return "instrucao privilegiada";
    default:                              return "excecao";
    }
}

static LONG CALLBACK mmio_veh(PEXCEPTION_POINTERS ep)
{
    /* Registradores de hardware interceptados (APU, AC'97, USB OHCI): o runtime
     * decodifica o acesso, emula e avanca o RIP. */
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            && xbox_VehMmioFault(ep))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI crash_filter(PEXCEPTION_POINTERS ep)
{
    static volatile LONG in_crash;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uintptr_t rip = (uintptr_t)ep->ContextRecord->Rip;
    uintptr_t fault = 0;

    if (InterlockedExchange(&in_crash, 1))
        return EXCEPTION_CONTINUE_SEARCH;   /* falha dentro do proprio relatorio */

    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION
            || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
            && er->NumberParameters >= 2)
        fault = (uintptr_t)er->ExceptionInformation[1];

    fprintf(stderr,
            "\n[CRASH] %s (0x%08lX) em RIP=0x%llx, endereco=0x%llx (VA guest 0x%08X)\n",
            exception_name(er->ExceptionCode), (unsigned long)er->ExceptionCode,
            (unsigned long long)rip, (unsigned long long)fault,
            (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
    dw4_crash_report_regs();
    fprintf(stderr, "  em: ");
    print_host_addr(rip);
    dw4_crash_report_guest_stack();

    fprintf(stderr, "  backtrace do host:\n");
    {
        CONTEXT c = *ep->ContextRecord;

        if (!c.Rip && c.Rsp) {              /* chamada por ponteiro nulo: quem chamou */
            c.Rip = *(DWORD64 *)c.Rsp;      /* esta no topo da pilha */
            c.Rsp += 8;
        }
        for (int i = 0; i < 48 && c.Rip; i++) {
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, NULL);

            fprintf(stderr, "    ");
            print_host_addr((uintptr_t)c.Rip);
            if (!fn) {                      /* funcao folha: o retorno esta no topo */
                c.Rip = *(DWORD64 *)c.Rsp;
                c.Rsp += 8;
            } else {
                PVOID handler_data;
                DWORD64 establisher;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fn, &c,
                                 &handler_data, &establisher, NULL);
            }
        }
    }
    dw4_hooks_crash_report();
    fflush(stderr);
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;       /* o sistema encerra o processo */
}

void dw4_os_install_crash_handlers(void)
{
    MEMORY_BASIC_INFORMATION mbi;
    HMODULE self = GetModuleHandleA(NULL);

    /* Limites da imagem do executavel, para imprimir enderecos relativos. */
    s_image_lo = (uintptr_t)self;
    s_image_hi = s_image_lo;
    while (VirtualQuery((void *)s_image_hi, &mbi, sizeof mbi) == sizeof mbi
            && mbi.AllocationBase == (void *)self)
        s_image_hi = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;

    AddVectoredExceptionHandler(1, mmio_veh);
    SetUnhandledExceptionFilter(crash_filter);
}

void dw4_dump_all_threads(void)
{
    fprintf(stderr, "[DUMP] despejo de threads ainda nao existe no Windows\n");
}

#endif /* _WIN32 */
