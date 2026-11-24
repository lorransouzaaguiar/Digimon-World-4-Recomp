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

#include <math.h>
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
/* DW4_RO_TRACE=<flip>[,<flip>...]: lista os lotes desses quadros (um quadro
 * de cada superficie: o flip dado e o seguinte). */
static inline int ro_trace_flip(uint64_t flip)
{
    const char *e = getenv("DW4_RO_TRACE");

    while (e && *e) {
        uint64_t f = strtoull(e, NULL, 10);
        if (f > 1 && flip >= f && flip < f + 2)
            return 1;
        e = strchr(e, ',');
        if (e) e++;
    }
    return 0;
}

/* ── Recorte (sub-viewport) ────────────────────────────────────────────────
 * O jogo desenha o minimapa das fases com a superficie recortada num
 * quadrado de 120x120 no canto: o conteudo e desenhado bem maior e a NV2A
 * corta o que sai do recorte. `clip` = {x, y, largura, altura} em pixels do
 * guest, ou tudo zero quando o recorte e a superficie inteira.
 *
 * Em tela larga um recorte estreito e tratado como UM elemento: tudo o que e
 * desenhado nele (e o proprio retangulo) passa pela mesma transformacao em
 * x. Encostado numa borda lateral da tela 4:3, fica ancorado na borda da
 * tela larga (o minimapa vai para o canto); senao e espremido em torno do
 * centro, como o resto. */
enum { RO_CLIP_NONE = 0, RO_CLIP_CENTER, RO_CLIP_LEFT, RO_CLIP_RIGHT };

static inline int ro_clip_mode(const uint32_t clip[4], float guest_w)
{
    float x0 = (float)clip[0], x1 = (float)(clip[0] + clip[2]);

    if (!clip[2] || (float)clip[2] >= guest_w * 0.9f)
        return RO_CLIP_NONE;                /* largura inteira: nao e um elemento */
    /* O minimapa fica em 468..588 de 640 (captura do usuario em 4:3): a
     * margem ate a borda e de 52 pixels, que a ancoragem preserva. */
    if (x1 >= guest_w * 0.85f && x0 > guest_w * 0.5f)
        return RO_CLIP_RIGHT;
    if (x0 <= guest_w * 0.15f && x1 < guest_w * 0.5f)
        return RO_CLIP_LEFT;
    return RO_CLIP_CENTER;
}

static inline float ro_clip_x(float x, int mode, float guest_w)
{
    const float k = 1.0f / ro_wide;

    switch (mode) {
    case RO_CLIP_LEFT:   return x * k;
    case RO_CLIP_RIGHT:  return guest_w - (guest_w - x) * k;
    case RO_CLIP_CENTER: return guest_w * 0.5f + (x - guest_w * 0.5f) * k;
    default:             return x;
    }
}

/* Retangulo de recorte em pixels da imagem da GPU (pw x ph) para uma
 * superficie do guest (gw x gh). 0 se nao ha o que recortar. */
static inline int ro_scissor(const uint32_t clip[4], int wide, uint32_t gw, uint32_t gh,
                             uint32_t pw, uint32_t ph, int32_t out[4])
{
    float x0, x1, y0, y1;
    int mode;

    if (!clip[2] || !clip[3] || !gw || !gh)
        return 0;
    x0 = (float)clip[0]; x1 = (float)(clip[0] + clip[2]);
    y0 = (float)clip[1]; y1 = (float)(clip[1] + clip[3]);
    if (wide) {
        mode = ro_clip_mode(clip, (float)gw);
        if (mode == RO_CLIP_NONE) {         /* so recorta na vertical */
            x0 = 0.0f; x1 = (float)gw;
        } else {
            x0 = ro_clip_x(x0, mode, (float)gw);
            x1 = ro_clip_x(x1, mode, (float)gw);
        }
    }
    x0 = x0 * (float)pw / (float)gw; x1 = x1 * (float)pw / (float)gw;
    y0 = y0 * (float)ph / (float)gh; y1 = y1 * (float)ph / (float)gh;
    if (x0 < 0.0f) x0 = 0.0f;
    if (y0 < 0.0f) y0 = 0.0f;
    if (x1 > (float)pw) x1 = (float)pw;
    if (y1 > (float)ph) y1 = (float)ph;
    out[0] = (int32_t)(x0 + 0.5f); out[1] = (int32_t)(y0 + 0.5f);
    out[2] = (int32_t)(x1 + 0.5f) - out[0]; out[3] = (int32_t)(y1 + 0.5f) - out[1];
    if (out[2] < 0) out[2] = 0;
    if (out[3] < 0) out[3] = 0;
    return 1;
}

/* textured: o lote tem textura (so para o rastreio DW4_RO_TRACE).
 * screen_tex: o lote amostra uma superficie de tela alargada -- uma copia do
 * proprio quadro (transicoes de loading, pausa). */
static inline void ro_squeeze(Nv2aVertex *v, uint32_t count, float guest_w, float guest_h,
                              int textured, int screen_tex, const uint32_t clip[4])
{
    const float k = 1.0f / ro_wide, cx = guest_w * 0.5f, e = 0.51f;
    static int trace = -1;

    {
        int mode = ro_clip_mode(clip, guest_w);

        if (mode != RO_CLIP_NONE) {         /* sub-viewport: um elemento so */
            for (uint32_t i = 0; i < count; i++)
                v[i].x = ro_clip_x(v[i].x, mode, guest_w);
            return;
        }
    }
    if (trace < 0)                      /* DW4_RO_TRACE=<flip>: lista os lotes desse quadro */
        trace = getenv("DW4_RO_TRACE") ? (atoi(getenv("DW4_RO_TRACE")) > 1
                                          ? atoi(getenv("DW4_RO_TRACE")) : 1) : 0;
    /* Regras do LOTE inteiro (draws seguidos com o mesmo estado; por isso
     * esta funcao e chamada no flush do lote, nao a cada draw).
     *
     * O fade do jogo ao trocar de cenario, e o escurecimento atras de alguns
     * menus, e uma malha 3D presa a camera: um plano de profundidade
     * constante (todo vertice com o mesmo rhw), centrado na tela, com o
     * miolo escuro do tamanho exato de uma tela 4:3 e uma moldura clara em
     * volta. Espremido junto com o cenario, o miolo virava um "quadrado
     * 4:3" escuro no meio e a moldura aparecia nas laterais (visto pelo
     * usuario em 16:9, 2026-11-24; achado com a caixa-preta: lote de 1086
     * vertices, rhw 0,00136..0,00138, caixa -190..831 x -318..799). Sem
     * espremer, o plano cobre a tela larga como cobria a de 4:3.
     *
     * O mesmo vale para uma malha 2D (rhw = 1) que cobre a tela inteira.
     * A area somada entra na conta: os elementos do HUD num lote so tambem
     * tem caixa de tela inteira, mas cobrem pouco. */
    if (count >= 6) {
        float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f, area = 0.0f;
        float r0 = 1e30f, r1 = -1e30f;

        for (uint32_t i = 0; i + 2 < count; i += 3) {
            const Nv2aVertex *t = &v[i];

            for (int j = 0; j < 3; j++) {
                if (t[j].x < bx0) bx0 = t[j].x;
                if (t[j].x > bx1) bx1 = t[j].x;
                if (t[j].y < by0) by0 = t[j].y;
                if (t[j].y > by1) by1 = t[j].y;
                if (t[j].rhw < r0) r0 = t[j].rhw;
                if (t[j].rhw > r1) r1 = t[j].rhw;
            }
            area += fabsf((t[1].x - t[0].x) * (t[2].y - t[0].y)
                        - (t[1].y - t[0].y) * (t[2].x - t[0].x)) * 0.5f;
        }
        if (trace > 1 && ro_trace_flip(dw4_frame_count()))
            fprintf(stderr, "[ROB] f%llu n=%u rhw %.5f..%.5f bb %.0f,%.0f-%.0f,%.0f area %.1f%% "
                    "tex=%d z %.1f\n", (unsigned long long)dw4_frame_count(), count, (double)r0,
                    (double)r1, (double)bx0, (double)by0, (double)bx1, (double)by1,
                    (double)(100.0f * area / (guest_w * guest_h)), textured, (double)v[0].z);
        if (r0 > 0.0f && r1 - r0 <= r1 * 0.03f
                && bx0 <= 1.0f && bx1 >= guest_w - 1.0f && by0 <= 1.0f && by1 >= guest_h - 1.0f
                && fabsf((bx0 + bx1) * 0.5f - cx) < guest_w * 0.1f
                && area >= guest_w * guest_h * 0.9f) {
            if (trace) {
                static unsigned n;
                if (n++ < 200)
                    fprintf(stderr, "[RO] lote de tela inteira n=%u rhw %.5f..%.5f bb %.0f,%.0f-"
                            "%.0f,%.0f area %.0f%% tex=%d/%d\n", count, (double)r0, (double)r1,
                            (double)bx0, (double)by0, (double)bx1, (double)by1,
                            (double)(100.0f * area / (guest_w * guest_h)), textured, screen_tex);
            }
            return;
        }
        /* HUD do jogo (retrato, HP/MP, nivel) em tela larga.
         *
         * Todo o 2D deste jogo tem rhw = 1. O HUD e uma FAIXA escura da
         * largura da tela encostada na borda de baixo (-18..659 x 393..485)
         * e, desenhados logo depois dela, os elementos de cada jogador. Em
         * 4:3 no meio de uma tela 16:9 a faixa acabava antes das bordas e o
         * retrato ficava longe do canto.
         *   - a faixa (cobre a largura, toca a borda de cima ou de baixo,
         *     baixa) nao e espremida: vai de borda a borda;
         *   - o lote 2D seguinte que cai na altura da faixa e fica todo de um
         *     lado da tela e ancorado na borda desse lado, com o tamanho
         *     certo (escala em x em torno da borda, nao do centro).
         * A caixa de dialogo nao encosta na borda (termina em y = 463) e
         * continua em 4:3, inteira, no meio; menus tambem. */
        if (r0 == 1.0f && r1 == 1.0f) {
            /* zona[0] = lado esquerdo, zona[1] = direito: a faixa de altura
             * em que um lote 2D daquele lado pertence ao HUD. Comeca na
             * faixa e CRESCE com cada lote ancorado: o menu rapido de itens
             * sobe a partir do HUD (painel 317..440), e a barra de selecao
             * dele (317..341) fica toda acima da faixa -- sem isto ela nao
             * acompanhava o painel e aparecia deslocada para a direita
             * (captura f9-014 do usuario). */
            static uint64_t band_frame;
            static float zone[2][2];
            uint64_t frame = dw4_frame_count();
            float bh = by1 - by0;

            if (bx0 <= 1.0f && bx1 >= guest_w - 1.0f && bh <= guest_h * 0.4f
                    && (by1 >= guest_h - 1.0f || by0 <= 1.0f)) {
                float m = bh * 0.5f;

                if (band_frame != frame + 1) {  /* +1: 0 = nenhuma faixa ainda */
                    band_frame = frame + 1;
                    zone[0][0] = zone[1][0] = by0 - m;
                    zone[0][1] = zone[1][1] = by1 + m;
                }
                return;
            }
            if (band_frame == frame + 1 && bx1 - bx0 < guest_w * 0.6f
                    && (bx1 <= cx || bx0 >= cx)) {
                int side = bx1 <= cx ? 0 : 1;

                if (by1 >= zone[side][0] && by0 <= zone[side][1]) {
                    if (by0 < zone[side][0]) zone[side][0] = by0;
                    if (by1 > zone[side][1]) zone[side][1] = by1;
                    for (uint32_t i = 0; i < count; i++)
                        v[i].x = side == 0 ? v[i].x * k : guest_w - (guest_w - v[i].x) * k;
                    return;
                }
            }
        }
    }
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
            /* A foto do proprio quadro (o jogo a desenha, com zoom, ao trocar
             * de cenario): a textura JA e a imagem larga. Espremido em x o
             * quadro mostrava a cena inteira num retangulo 4:3 no meio, que
             * depois "crescia" para a tela larga. Sem espremer, a foto ocupa
             * na tela larga a mesma fracao que ocupava na de 4:3 e mantem a
             * proporcao da cena. */
            if (screen_tex)
                continue;
            /* Fade / escurecimento do jogo: um quadro da altura da tela, um
             * pouco mais estreito que ela (29,6..611,4 de 640 -- na TV a
             * sobra ficava no overscan) e que amostra UM ponto da textura
             * (a cor vem de um texel, a matriz de textura anda sobre um
             * gradiente; toolkit, patch 4.8). Espremido para o meio ele
             * virava um "quadrado 4:3" escuro com o cenario aparecendo nas
             * laterais. Por ser de uma cor so, pode cobrir a tela larga
             * inteira sem deformar nada. Um quadro com imagem (uv variando)
             * continua em 4:3. */
            if (y0 <= e && y1 >= guest_h - e && x0 <= guest_w * 0.15f
                    && x1 >= guest_w * 0.85f
                    && fabsf(t[0].u - t[1].u) < 1e-3f && fabsf(t[0].u - t[2].u) < 1e-3f
                    && fabsf(t[0].v - t[1].v) < 1e-3f && fabsf(t[0].v - t[2].v) < 1e-3f) {
                float half = cx - x0 < x1 - cx ? cx - x0 : x1 - cx;
                float f = half > 1.0f ? cx / half : 1.0f;

                if (f > 1.0f)
                    for (int j = 0; j < 3; j++)
                        t[j].x = cx + (t[j].x - cx) * f;
                continue;
            }
        }
        for (int j = 0; j < 3; j++)
            t[j].x = cx + (t[j].x - cx) * k;
    }
}

#endif /* DW4_RENDER_OPTS_H */
