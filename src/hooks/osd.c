/**
 * Mostrador de desempenho: FPS, uso de CPU e uso de GPU no canto da tela.
 *
 *   DW4_OSD=1                 ja abre visivel (o launcher grava)
 *   DW4_OSD_KEYS=f2           tecla(s) que ligam/desligam    (quit_hotkey.c)
 *   DW4_OSD_PAD=l3+r3         botoes do controle que ligam/desligam
 *
 * O texto nao tem renderizador proprio: e um lote 2D sem textura (um
 * retangulo por trecho aceso de uma fonte 5x7) entregue ao renderizador em
 * uso logo antes do fim do quadro (gl_host.c), pelo mesmo caminho dos lotes do
 * jogo. Por isso sai igual em Vulkan e OpenGL, em qualquer resolucao. Em tela
 * larga o 2D e espremido para a coluna 4:3 do meio (render_opts.h); o
 * mostrador compensa isso e fica no canto de cima, a esquerda, da tela.
 * Com o rasterizador em CPU (DW4_SW_RENDER=1) nao aparece.
 *
 * Numeros, atualizados duas vezes por segundo pela thread dos atalhos:
 *   FPS  quadros apresentados por segundo
 *   CPU  ocupacao media de todos os nucleos do computador (0..100), como no
 *        monitor do sistema; inclui os outros programas (pedido do usuario,
 *        2026-11-25: antes era o tempo so do jogo, em % de um nucleo)
 *   GPU  ocupacao da placa de video inteira, nao so do jogo; "--" quando o
 *        sistema nao informa (host_os.h: dw4_os_gpu_percent)
 */

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "dw4_hooks.h"
#include "host_os.h"

static SDL_atomic_t s_on, s_fps10, s_cpu, s_gpu;

/* Fonte 5x7: uma linha por byte, bit 4 = coluna da esquerda. */
static const struct { char c; uint8_t rows[7]; } s_font[] = {
    { '0', { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E } },
    { '1', { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E } },
    { '2', { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F } },
    { '3', { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E } },
    { '4', { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 } },
    { '5', { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E } },
    { '6', { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E } },
    { '7', { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 } },
    { '8', { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E } },
    { '9', { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C } },
    { 'F', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 } },
    { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } },
    { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { 'G', { 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F } },
    { '%', { 0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03 } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } },
    { '.', { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C } },
};

#define OSD_MAX_VERTS 4096
static Nv2aVertex s_verts[OSD_MAX_VERTS];
static Nv2aRenderState s_state;

static uint32_t rect(uint32_t n, float x0, float y0, float x1, float y1, uint32_t argb)
{
    static const int corner[6][2] = { {0,0}, {1,0}, {1,1}, {0,0}, {1,1}, {0,1} };

    if (n + 6 > OSD_MAX_VERTS)
        return n;
    for (int i = 0; i < 6; i++) {
        Nv2aVertex *v = &s_verts[n + i];

        v->x = corner[i][0] ? x1 : x0;
        v->y = corner[i][1] ? y1 : y0;
        v->z = 0.0f;
        v->rhw = 1.0f;
        v->diffuse = argb;
        v->u = v->v = 0.0f;
    }
    return n + 6;
}

/* Um texto em (x, y), ponto de `px` pixels; trechos acesos seguidos viram um retangulo so. */
static uint32_t text(uint32_t n, const char *s, float x, float y, float px, float py, uint32_t argb)
{
    for (; *s; s++, x += 6.0f * px) {
        const uint8_t *rows = NULL;

        for (size_t i = 0; i < sizeof s_font / sizeof s_font[0]; i++)
            if (s_font[i].c == *s)
                rows = s_font[i].rows;
        for (int r = 0; rows && r < 7; r++)
            for (int c = 0; c < 5; c++) {
                int c0 = c;

                if (!(rows[r] & (0x10 >> c)))
                    continue;
                while (c + 1 < 5 && (rows[r] & (0x10 >> (c + 1))))
                    c++;
                n = rect(n, x + (float)c0 * px, y + (float)r * py,
                         x + (float)(c + 1) * px, y + (float)(r + 1) * py, argb);
            }
    }
    return n;
}

int dw4_osd_visible(void)
{
    return SDL_AtomicGet(&s_on);
}

void dw4_osd_toggle(void)
{
    int on = !SDL_AtomicGet(&s_on);

    SDL_AtomicSet(&s_on, on);
    fprintf(stderr, "[OSD] mostrador de desempenho %s\n", on ? "ligado" : "desligado");
}

void dw4_osd_init(void)
{
    const char *e = getenv("DW4_OSD");

    SDL_AtomicSet(&s_gpu, -1);
    SDL_AtomicSet(&s_on, e && *e && *e != '0');
}

/* Chamado pela thread dos atalhos (quit_hotkey.c) a cada volta: refaz os
 * numeros quando passou meio segundo. Fora da thread do renderizador porque
 * perguntar a ocupacao da GPU ao sistema pode demorar. */
void dw4_osd_sample(void)
{
    static Uint32 t_last;
    static uint64_t flips_last;
    static double busy_last, total_last;
    Uint32 now = SDL_GetTicks();
    uint64_t flips;
    double busy = 0.0, total = 0.0, dt;
    int cpu_ok;

    if (!SDL_AtomicGet(&s_on)) {
        t_last = 0;                     /* ao religar, comeca uma medida nova */
        return;
    }
    if (t_last && now - t_last < 500)
        return;
    flips = dw4_frame_count();
    cpu_ok = dw4_os_cpu_times(&busy, &total);
    if (t_last) {
        dt = (double)(now - t_last) / 1000.0;
        SDL_AtomicSet(&s_fps10, (int)((double)(flips - flips_last) * 10.0 / dt + 0.5));
        SDL_AtomicSet(&s_cpu, cpu_ok && total > total_last
                              ? (int)((busy - busy_last) * 100.0 / (total - total_last) + 0.5) : -1);
        SDL_AtomicSet(&s_gpu, dw4_os_gpu_percent());
    }
    t_last = now ? now : 1;
    flips_last = flips;
    busy_last = busy;
    total_last = total;
}

/* O lote do mostrador para a superficie de tela `s` (onde o jogo acabou de
 * desenhar), partindo do estado `base` do ultimo lote dela. 0 = nada a desenhar. */
int dw4_osd_batch(const Nv2aSurface *s, const Nv2aRenderState *base, Nv2aBatch *out)
{
    /* Coordenadas em pixels reais da superficie: os de 640x480 vezes o AA. */
    float ax = (float)(s->aa_sx ? s->aa_sx : 1), ay = (float)(s->aa_sy ? s->aa_sy : 1);
    float px = 2.0f * ax, py = 2.0f * ay, x = 14.0f * ax, y = 12.0f * ay;
    static float wide = 0.0f;
    char line[3][16];
    int fps10 = SDL_AtomicGet(&s_fps10), cpu = SDL_AtomicGet(&s_cpu), gpu = SDL_AtomicGet(&s_gpu);
    uint32_t n = 0;
    size_t cols = 0;

    if (!SDL_AtomicGet(&s_on))
        return 0;
    if (wide == 0.0f) {                 /* a mesma conta de ro_init (render_opts.h) */
        const char *asp = getenv("DW4_ASPECT");
        float a = 0.0f, b = 0.0f;

        wide = 1.0f;
        if (asp && sscanf(asp, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) {
            float w = (a / b) / (4.0f / 3.0f);
            if (w >= 1.01f && w <= 3.0f)
                wide = w;
        }
    }
    /* Em tela larga o renderizador espreme o 2D em torno do centro (coluna
     * 4:3). Para o mostrador ficar no canto da TELA, o x sai deslocado do
     * tanto que o aperto vai devolver: x = x_tela - 320 * (wide - 1). */
    x -= 320.0f * ax * (wide - 1.0f);
    snprintf(line[0], sizeof line[0], "FPS %d", (fps10 + 5) / 10);
    if (cpu >= 0)
        snprintf(line[1], sizeof line[1], "CPU %d%%", cpu > 100 ? 100 : cpu);
    else
        snprintf(line[1], sizeof line[1], "CPU --");
    if (gpu >= 0)
        snprintf(line[2], sizeof line[2], "GPU %d%%", gpu > 100 ? 100 : gpu);
    else
        snprintf(line[2], sizeof line[2], "GPU --");
    for (int i = 0; i < 3; i++)
        if (strlen(line[i]) > cols)
            cols = strlen(line[i]);
    n = rect(n, x - 4.0f * ax, y - 4.0f * ay, x + ((float)cols * 6.0f - 1.0f) * px + 4.0f * ax,
             y + 3.0f * 10.0f * py - 3.0f * py + 4.0f * ay, 0xB0000000u);
    for (int i = 0; i < 3; i++)
        n = text(n, line[i], x, y + (float)i * 10.0f * py, px, py, i ? 0xFFFFFFFFu : 0xFFFFD23Fu);

    s_state = *base;
    s_state.blend_enable = 1;
    s_state.blend_src = 0x302;          /* GL_SRC_ALPHA */
    s_state.blend_dst = 0x303;          /* GL_ONE_MINUS_SRC_ALPHA */
    s_state.blend_eq = 0x8006;          /* GL_FUNC_ADD */
    s_state.alpha_test_enable = 0;
    s_state.depth_test_enable = 0;
    s_state.depth_write = 0;
    s_state.stencil_enable = 0;
    s_state.cull_enable = 0;
    s_state.poly_offset_enable = 0;
    s_state.color_mask = 0x01010101;
    memset(out, 0, sizeof *out);
    out->vertices = s_verts;
    out->count = n;
    out->state = &s_state;
    return n > 0;
}
