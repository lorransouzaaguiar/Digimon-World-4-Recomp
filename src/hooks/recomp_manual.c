/**
 * Overrides manuais e diagnostico de ICALL do Digimon World 4.
 *
 *   recomp_lookup_manual()      intercepta VAs do guest com codigo escrito a mao
 *   recomp_icall_fail_log()     alvo de chamada indireta sem entrada no dispatch
 *   recomp_icall_not_code_log() alvo que nao e codigo (ponteiro nulo/selvagem)
 *   recomp_unimpl()             instrucao sem traducao que foi ALCANCADA
 *
 * Os registradores sao thread-local: sempre via recomp_types.h, nunca com
 * `extern uint32_t g_eax` solto (seria outra variavel -- ver 05-runtime.md).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"
#include "dw4_hooks.h"

/* ── Defaults de ambiente ──────────────────────────────────────────────────── */

static void env_default(const char *name, const char *value)
{
    if (!getenv(name))
        setenv(name, value, 0);
}

void dw4_hooks_set_env_defaults(void)
{
    /* O jogo linka o D3D8 do XDK estaticamente: ele nunca chama uma API D3D,
     * escreve metodos NV2A num pushbuffer. Para ver frames e preciso executar
     * esse pushbuffer e abrir a janela do framebuffer. */
    env_default("RECOMP_PB_EXEC", "1");
    env_default("RECOMP_FB_WINDOW", "1");
    /* O D3D do XDK espera pelo vblank para o swap. */
    env_default("RECOMP_VBLANK", "paced");
    /* Segura cada flip na contagem de vblank (60 Hz). O rasterizador em CPU
     * era lento o bastante para esconder a falta disso; com a GPU o jogo
     * passava de 2000 fps, a logica corria junto e o processo caia por
     * corrupcao de heap em poucos segundos. */
    env_default("RECOMP_FLIP_PACING", "1");
    /* O alocador do Alchemy (sbrk estilo dlmalloc sobre VirtualQuery/VirtualAlloc,
     * sub_0006F7F0) percorre o espaco de enderecos atras de uma regiao MEM_FREE
     * grande e reserva naquele endereco exato. Sem o rastreador de VMA acima
     * dos mirrors toda regiao livre tem 4 KB e a varredura nunca termina. */
    env_default("RECOMP_EXT_VMA", "1");
    /* O XBE nao importa KeInitializeEvent: o D3D do XDK monta o KEVENT do
     * vblank a mao (device+0x1DBC) e BlockUntilVerticalBlank (0x002ABBC0)
     * espera nele. Sem evento de host a espera falha na hora e as threads do
     * CRI (vsync 0x0024D910 / 0x0024D990) giram a ~100 mil voltas/s. */
    env_default("RECOMP_TITLE_KEVENTS", "1");
    /* O DirectSound do XDK (secao DSOUND) fala direto com a APU/AC'97. Sem o
     * codec "pronto" o DirectSoundCreate falha e a thread principal fica presa
     * em 0x002E76A5 logo apos a tela de Loading. Com isto o runtime intercepta
     * os registradores e os entrega a APU emulada (saida via SDL2). */
    env_default("RECOMP_AC97_READY", "1");
    /* O XAPI linkado no jogo enumera o controle direto nos registradores OHCI
     * (USB). O modelo do toolkit e opt-in; sem ele nao existe controle na
     * porta 0 -- nem gamepad SDL, nem o teclado do RECOMP_KEYBOARD. */
    /* Tiras de triangulos: o executor emite (i, i+1, i+2) para todo i, entao
     * os triangulos impares saem com o giro trocado. Com culling na GPU isso
     * descartava metade de cada tira (modelos 3D "salpicados" de preto na
     * tela de selecao). Esta opcao faz todos girarem como no hardware. */
    env_default("RECOMP_STRIP_ALT_WINDING", "1");
    env_default("RECOMP_USB", "1");
    /* O alocador contiguo comecava em fisico 0x1000, dentro da faixa numerica
     * da imagem (0x10000..0x4FD000). O driver USB entrega enderecos fisicos
     * ao controlador (HCCA 0x00082000, EDs 0x000826A0) e o modelo OHCI os
     * tomava por enderecos da imagem -- lia o .text do jogo como descritores
     * e a enumeracao do controle nunca terminava. */
    env_default("RECOMP_CONTIG_AVOID_IMAGE", "1");
    /* Com o renderizador Vulkan (o padrao) a janela e aberta para uma
     * superficie Vulkan: o back end apresenta por swapchain e o frame nao
     * sai da GPU (patch 4.14 no toolkit; nv2a_vk_backend.c "Apresentacao
     * direta"). Com OpenGL ou CPU, a janela SDL de sempre. Se o Vulkan nao
     * subir, a janela ganha o renderizador SDL no primeiro frame. */
    {
        const char *api = getenv("DW4_RENDERER"), *sw = getenv("DW4_SW_RENDER");
        const char *pm = getenv("DW4_VK_PRESENT");
        int vulkan = !(api && *api && strcmp(api, "vulkan") && strcmp(api, "vk"))
                  && !(sw && *sw && *sw != '0');

        if (vulkan && !(pm && !strcmp(pm, "readback")))
            env_default("RECOMP_WINDOW_VULKAN", "1");
    }
}

void dw4_hooks_init(void)
{
    dw4_gl_backend_init();
    dw4_d3d_fence_init();
    dw4_screenshot_init();
    dw4_input_script_init();
    dw4_stall_watch_init();
    dw4_prof_init();
}

void dw4_hooks_crash_report(void)
{
}

/* ── Overrides ─────────────────────────────────────────────────────────────── */

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── Falhas de ICALL ───────────────────────────────────────────────────────── */

void recomp_icall_fail_log(uint32_t va)
{
    static int budget = 200;

    if (budget <= 0)
        return;
    budget--;
    /* Formato do toolkit: tools.seed_from_log casa "Failed to resolve VA". */
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (calls: %llu, "
                    "guest ret 0x%08X)\n",
            va, (unsigned long long)g_icall_count, MEM32(g_esp));
    fprintf(stderr, "  ICALLs recentes:");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + i) & 15]);
    fprintf(stderr, "\n");
}

void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Reporta em 1, 10, 100, ...: a progressao diferencia curiosidade de
     * laco travado pulando um ponteiro nulo. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] alvo 0x%08X nao e codigo -- pulado %llu vez(es) "
                    "(ponteiro nulo/selvagem, chamada #%llu, ret guest 0x%08X)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count, MEM32(g_esp));
}

/* ── Instrucoes nao traduzidas ─────────────────────────────────────────────── */

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr, "[UNIMPL] instrucao nao traduzida ALCANCADA: `%s` em "
                        "0x%08X (no-op; RECOMP_UNIMPL_TRAP=1 para parar aqui)\n",
                text, va);
    }
    if (stop)
        abort();
}

/* ── Ganchos de entrada ───────────────────────────────────────────────────── *
 * `void sub_XXXXXXXX_enter(void)` e chamado no topo da funcao gerada de mesmo
 * endereco. O recompilador so emite a chamada se enxergar a definicao aqui:
 * scripts/regen.sh passa este arquivo em --exclude-manual. Depois de criar ou
 * remover um gancho: scripts/regen.sh --lift e recompilar tudo. */

/* 0x000BC050: teste de descarte do igFrustCullTraversal. ecx = 8 cantos da
 * caixa em espaco de clip (x, y, z, w). Em widescreen o x e encolhido para o
 * teste aceitar o que cai na faixa lateral nova (ver widescreen.c). */
void sub_000BC050_enter(void)
{
    float k = dw4_cull_x_scale();

    if (k == 1.0f)
        return;
    for (uint32_t i = 0; i < 8; i++)
        MEMF(g_ecx + i * 16) *= k;
}

/* ── Funcoes embrulhadas ──────────────────────────────────────────────────── *
 * `extern void sub_XXXXXXXX_gen(void);` + uma definicao de sub_XXXXXXXX aqui:
 * o recompilador emite o corpo gerado com o nome _gen e todo chamador passa
 * por esta versao. Mesma regra dos ganchos: regen.sh --lift + build completo. */

/* 0x002B1320: kick-off do D3D (XDK). Liga o bit 16 de NV_PFB 0x100410, gira
 * ate a GPU limpa-lo e so entao grava o DMA_PUT. A GPU emulada so limpa o bit
 * entre duas caminhadas do pushbuffer, e nao limpa com um flip retido a
 * espera do vblank: a thread do jogo queimava um nucleo inteiro girando ali
 * (86% das amostras dela no lobby, DW4_PROF). O portao dorme ate a GPU poder
 * responder; o corpo gerado roda igual. Ver xbox_Nv2aKickGate (PORT_NOTES
 * §4.13). DW4_KICK_GATE=0 volta a espera ocupada. */
extern void sub_002B1320_gen(void);

void sub_002B1320(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("DW4_KICK_GATE");
        on = !(e && e[0] == '0');
    }
    if (!on) {
        sub_002B1320_gen();
        return;
    }
    xbox_Nv2aKickGate();
    sub_002B1320_gen();
    xbox_Nv2aKickDone();
}
