/**
 * Widescreen: alarga o descarte (culling) de objetos do jogo.
 *
 * O renderizador faz o 16:9 sozinho (render_opts.h): a superficie fica mais
 * larga e a geometria e comprimida em x, entao o que o jogo desenhava alem das
 * bordas de 640 aparece nas laterais. Mas o jogo so MANDA desenhar o que cabe
 * no campo de visao de 4:3: objetos inteiros dentro da faixa lateral nova
 * sumiam e surgiam conforme a camera andava (relatado pelo usuario em
 * 2026-11-24, capturas f9-011/f9-012: o piso da plataforma a direita).
 *
 * O teste do motor (Alchemy, igFrustCullTraversal) e sub_000BC050: recebe em
 * ecx os 8 cantos da caixa do objeto ja em espaco de clip (x, y, z, w; 16
 * bytes cada) e devolve 2 = fora, 1 = dentro, 3/5 = parcial. Um gancho na
 * entrada dessa funcao (sub_000BC050_enter, em recomp_manual.c) multiplica o
 * x dos cantos por dw4_cull_x_scale() = 1 / (alargamento x folga): para o
 * teste, o campo de visao fica tao largo quanto a tela. Os cantos sao um
 * temporario na pilha de quem chama (sub_000BCC80, sub_000D7EC0); a projecao
 * e o desenho nao mudam.
 *
 * Descartado: igFrustCullTraversal::_outerFrustCullScale (global 0x00397A90).
 * Parecia ser isto, mas so reclassifica o caso "parcial" (3 -> 5); quem esta
 * fora do campo de visao real continua fora. Testado e nao resolveu.
 *
 *   DW4_CULL_SCALE=<f>   forca o alargamento do teste (1 = jogo original)
 */

#include <stdio.h>
#include <stdlib.h>

#include "dw4_hooks.h"
#include "render_opts.h"

float dw4_cull_x_scale(void)
{
    static float k;

    if (k == 0.0f) {                    /* corrida benigna: mesmo valor em toda thread */
        const char *r = getenv("DW4_RENDERER"), *sw = getenv("DW4_SW_RENDER");
        const char *e = getenv("DW4_CULL_SCALE");
        float scale;

        ro_init(NULL);
        scale = ro_wide > 1.0f ? ro_wide * 1.05f : 1.0f;
        /* o rasterizador em CPU ignora a proporcao: nada a alargar */
        if ((sw && *sw && *sw != '0') || (r && (!strcmp(r, "cpu") || !strcmp(r, "sw"))))
            scale = 1.0f;
        if (e && atof(e) >= 1.0 && atof(e) <= 8.0)
            scale = (float)atof(e);
        if (scale != 1.0f)
            fprintf(stderr, "[WS] descarte de objetos do jogo alargado x%.3f em x "
                            "(sub_000BC050)\n", (double)scale);
        k = 1.0f / scale;
    }
    return k;
}
