/**
 * Resolucao interna e proporcao da imagem -- comum aos dois renderizadores
 * (nv2a_gl_backend.c e nv2a_vk_backend.c). Tudo `static`: cada modulo tem a
 * sua copia; mude aqui e os dois mudam juntos.
 *
 *   DW4_RES=<altura>      altura da imagem em pixels: 480 (original), 720,
 *                         1080, 1440, 2160... A escala e altura/480 e vale
 *                         para todas as superficies (alvos de render-to-
 *                         texture tambem).
 *   DW4_ASPECT=<L:A>      16:9, 16:10, 21:9, qualquer L:A; 4:3 = original.
 *
 * Desenho (o mesmo do Amped 2 e do SSX 3, docs/REFERENCIAS.md; codigo nosso):
 *
 *   resolucao  a imagem da GPU tem o tamanho da superficie do guest x escala.
 *              Os vertices continuam em pixels do guest e o shader divide
 *              pelo tamanho do guest, entao so mudam o tamanho das imagens e
 *              o viewport.
 *   proporcao  as superficies "de tela" (640 de largura) ficam `wide` vezes
 *              mais largas e todo vertice desenhado nelas tem o x comprimido
 *              em torno do centro por 1/wide. O que o jogo desenhava alem das
 *              bordas de 640 aparece nas laterais (Hor+: mais cenario, sem
 *              esticar; o executor so recorta no near plane). HUD, menus e
 *              videos sao 2D e ficam em 4:3 no meio. Excecao: um triangulo 2D
 *              que cobre a tela inteira (fade, clarao, pos-processamento)
 *              continua cobrindo a tela inteira.
 *
 * Com 480 e 4:3 nada disto entra em acao e o caminho e o de sempre: o frame
 * volta para a memoria do guest e a janela mostra os 640x480 dela. Fora
 * disso o frame vai direto para a janela (xbox_FramebufferWindowPresentPixels,
 * patch 4.10 no toolkit) e para as capturas (dw4_frame_note_host).
 */
#ifndef DW4_RENDER_OPTS_H
#define DW4_RENDER_OPTS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

static float ro_scale = 1.0f;           /* altura / 480 */
static float ro_wide = 1.0f;            /* proporcao / (4/3) */

static inline int ro_enhanced(void)
{
    return ro_scale != 1.0f || ro_wide != 1.0f;
}

static inline void ro_init(const char *tag)
{
    const char *res = getenv("DW4_RES"), *asp = getenv("DW4_ASPECT");
    float a = 0.0f, b = 0.0f;

    ro_scale = 1.0f;
    ro_wide = 1.0f;
    if (res && atoi(res) >= 240 && atoi(res) <= 4320)
        ro_scale = (float)atoi(res) / 480.0f;
    if (asp && sscanf(asp, "%f:%f", &a, &b) == 2 && a > 0.0f && b > 0.0f) {
        float w = (a / b) / (4.0f / 3.0f);
        if (w >= 0.99f && w <= 3.0f)    /* ate 32:9; mais estreito que 4:3 nao */
            ro_wide = w < 1.01f ? 1.0f : w;
    }
    if (tag && ro_enhanced())
        fprintf(stderr, "[%s] imagem: %dx%d (escala %.2fx, proporcao %.3f:1)\n", tag,
                (int)(640.0f * ro_scale * ro_wide + 0.5f), (int)(480.0f * ro_scale + 0.5f),
                (double)ro_scale, (double)(ro_wide * 4.0f / 3.0f));
}

/* Superficie "de tela": a que o jogo exibe (640 de largura). Alvos de
 * render-to-texture (512x512, 256x256...) nao sao alargados.
 * Sem limite superior na altura de proposito: ao entrar no lobby o jogo faz
 * uma passada com recorte 512x512 ainda apontando para o back buffer, e a
 * superficie passa a ser 640x512 para sempre. Com o limite em 480 ela deixava
 * de ser "de tela" e, conforme qual dos dois buffers estava na vez, a imagem
 * saia em 4:3 (visto em 2026-11-24). */
static inline int ro_is_screen(uint32_t guest_w, uint32_t guest_h)
{
    return ro_wide != 1.0f && guest_w == 640 && guest_h >= 400;
}

/* Linhas do guest que vao para a tela (a superficie pode ter mais, ver acima). */
static inline uint32_t ro_display_rows(uint32_t guest_h)
{
    return guest_h > 480 ? 480 : guest_h;
}

/* Tamanho da imagem da GPU para uma superficie do guest. */
static inline void ro_extent(uint32_t guest_w, uint32_t guest_h, int wide,
                             uint32_t *pw, uint32_t *ph)
{
    float w = (float)guest_w * ro_scale * (wide ? ro_wide : 1.0f);
    float h = (float)guest_h * ro_scale;

    *pw = w < 1.0f ? 1 : (uint32_t)(w + 0.5f);
    *ph = h < 1.0f ? 1 : (uint32_t)(h + 0.5f);
}

/* Comprime em x, em torno do centro, os vertices de um lote desenhado numa
 * superficie alargada (ver o topo do arquivo). `v` e a copia do back end. */
static inline void ro_squeeze(Nv2aVertex *v, uint32_t count, float guest_w, float guest_h)
{
    const float k = 1.0f / ro_wide, cx = guest_w * 0.5f, e = 0.51f;

    for (uint32_t i = 0; i + 2 < count; i += 3) {
        Nv2aVertex *t = &v[i];

        if (t[0].rhw == 1.0f && t[1].rhw == 1.0f && t[2].rhw == 1.0f) {
            float x0 = t[0].x, x1 = t[0].x, y0 = t[0].y, y1 = t[0].y;

            for (int j = 1; j < 3; j++) {
                if (t[j].x < x0) x0 = t[j].x;
                if (t[j].x > x1) x1 = t[j].x;
                if (t[j].y < y0) y0 = t[j].y;
                if (t[j].y > y1) y1 = t[j].y;
            }
            /* 2D que cobre a tela inteira: fica como esta (estica junto) */
            if (x0 <= e && x1 >= guest_w - e && y0 <= e && y1 >= guest_h - e)
                continue;
        }
        for (int j = 0; j < 3; j++)
            t[j].x = cx + (t[j].x - cx) * k;
    }
}

#endif /* DW4_RENDER_OPTS_H */
