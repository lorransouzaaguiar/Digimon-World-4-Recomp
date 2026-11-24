/**
 * Hooks especificos do Digimon World 4 (ver PORT_NOTES.md).
 */
#ifndef DW4_HOOKS_H
#define DW4_HOOKS_H

#include <stdint.h>

/* Log que dispara uma unica vez por ponto de chamada. */
#define LOG_ONCE(...) do {                    \
        static int _dw4_logged;               \
        if (!_dw4_logged) {                   \
            _dw4_logged = 1;                  \
            fprintf(stderr, __VA_ARGS__);     \
        }                                     \
    } while (0)

/* Defaults de ambiente do port (so define o que o usuario nao definiu).
 * Chamado antes de qualquer init do runtime. */
void dw4_hooks_set_env_defaults(void);

/* Chamado depois do kernel/bridge prontos, antes do entry point. */
void dw4_hooks_init(void);

/* Diagnostico extra no relatorio de crash. */
void dw4_hooks_crash_report(void);

/* d3d_fence.c: informa ao executor de pushbuffer onde fica o semaforo do D3D. */
void dw4_d3d_fence_init(void);

/* screenshot.c: frame exibido. DW4_SHOT=<prefixo> grava BMPs periodicamente. */
#define DW4_SIG_W 16
#define DW4_SIG_H 12
#define DW4_SIG_CELLS (DW4_SIG_W * DW4_SIG_H)
void dw4_screenshot_init(void);
/* Chamado a cada flip pelo back end: onde esta o frame pronto (0 = nao sei). */
void dw4_frame_note(uint32_t va, uint32_t pitch);
/* Frame que o renderizador entrega direto, de qualquer tamanho (B,G,R,X, linha
 * de cima primeiro); NULL = voltar a ler a memoria do guest. O buffer e do
 * renderizador: avise com NULL ANTES de libera-lo. */
void dw4_frame_note_host(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t pitch);
/* Caixa-preta (screenshot.c): etiqueta do proximo quadro, e gravar agora os
 * ultimos ~2 s de quadros (F12). A gravacao automatica e por clarao detectado. */
void dw4_frame_tag(const char *tag);
void dw4_frame_ring_dump(void);
/* Uma linha de texto por lote do quadro corrente; sai em lotes.txt na gravacao. */
void dw4_frame_detail(const char *line);
/* 1 com DW4_FRAME_RING=1 (desligada por padrao): so entao vale montar as
 * linhas de dw4_frame_detail. */
int dw4_frame_ring_on(void);
/* Captura sob demanda (renderizador que apresenta direto pela GPU): ele liga
 * com dw4_frame_on_demand(1) e pergunta a cada quadro se alguem quer pixels;
 * se sim, entrega aquele quadro por dw4_frame_note_host. */
void dw4_frame_on_demand(int on);
int dw4_frame_wanted(void);
uint64_t dw4_frame_count(void);
/* Grade 16x12 de luminancia media do frame exibido; 0 se ainda nao ha frame. */
int dw4_frame_signature(uint8_t sig[DW4_SIG_CELLS]);
int dw4_screenshot_save(const char *path);
/* O mesmo sem esperar um quadro novo (para a thread do renderizador). */
int dw4_screenshot_save_now(const char *path);

/* widescreen.c: fator (<= 1) aplicado ao x dos cantos no teste de descarte de
 * objetos do jogo; 1 fora do widescreen. Usado por sub_000BC050_enter. */
float dw4_cull_x_scale(void);

/* fps_limit.c: a cada flip; DW4_FPS_LIMIT=30 segura o flip. */
void dw4_fps_limit_tick(void);

/* input_script.c: DW4_INPUT_SCRIPT="sleep:17 enter! z! quit" ou @arquivo. */
void dw4_input_script_init(void);

/* stall_watch.c: DW4_STALL_MS sem flip -> despeja todas as threads no log. */
void dw4_stall_watch_init(void);
void dw4_dump_all_threads(void);

/* prof.c: amostrador de CPU (DW4_PROF=<arquivo>); dump ao sair. */
void dw4_prof_init(void);
void dw4_prof_dump(void);

/* nv2a_gl_backend.c: back end OpenGL 3.3 do executor NV2A (DW4_SW_RENDER=1 desliga). */
void dw4_gl_backend_init(void);

#endif /* DW4_HOOKS_H */
