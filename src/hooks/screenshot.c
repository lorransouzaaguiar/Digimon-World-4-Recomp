/**
 * Acesso ao frame exibido: captura em BMP e "assinatura" de baixa resolucao.
 *
 *   DW4_SHOT=<prefixo>      grava <prefixo>NNN.bmp periodicamente
 *   DW4_SHOT_EVERY=<seg>    intervalo (padrao 5 s)
 *
 * Fonte do frame: a superficie que o back end OpenGL acabou de trazer para a
 * memoria do guest no flip (dw4_frame_note), ou, sem back end, o endereco de
 * scanout (PCRTC_START / AvSetDisplayMode). 640x480, 32 bpp.
 *
 * A assinatura (grade 16x12 de luminancia media) e o que o roteiro de entrada
 * usa para saber se a tela mudou (input_script.c) -- sem depender de tempo.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xbox_memory_layout.h"
#include "dw4_hooks.h"

#define SHOT_W 640u
#define SHOT_H 480u

static const char *s_prefix;
static double s_every = 5;

/* Ultimo frame apresentado (guest VA resolvido) e contagem de flips. */
static volatile uint32_t s_frame_va, s_frame_pitch;
static volatile uint64_t s_frame_count;

void dw4_frame_note(uint32_t va, uint32_t pitch)
{
    if (va) {
        s_frame_va = va;
        s_frame_pitch = pitch;
    }
    __atomic_add_fetch(&s_frame_count, 1, __ATOMIC_RELAXED);
}

uint64_t dw4_frame_count(void)
{
    return __atomic_load_n(&s_frame_count, __ATOMIC_RELAXED);
}

/* Ponteiro do host para o frame exibido, ou NULL. */
static const uint8_t *frame_ptr(uint32_t *pitch)
{
    uint32_t va = s_frame_va, p = s_frame_pitch;

    if (!va) {
        volatile uint32_t *start = xbox_Nv2aRegPtr(0x600800);
        uint32_t pa = start ? (*start & 0x03FFFFFFu) : 0;
        uint32_t fb = xbox_GetDisplayFramebuffer(&p) & 0x03FFFFFFu;

        if (!pa)
            pa = fb;
        if (!pa)
            return NULL;
        va = 0x80000000u + pa;          /* RAM fisica contigua */
    }
    if (!p)
        p = SHOT_W * 4;
    if (va >= 0x80000000u) {
        if ((uint64_t)va + (uint64_t)p * SHOT_H > 0x84000000ull)
            return NULL;
    } else if ((uint64_t)va + (uint64_t)p * SHOT_H > xbox_GetMappedSize()) {
        return NULL;
    }
    *pitch = p;
    return (const uint8_t *)xbox_GetMemoryOffset() + va;
}

int dw4_frame_signature(uint8_t sig[DW4_SIG_CELLS])
{
    uint32_t pitch = 0;
    const uint8_t *fb = frame_ptr(&pitch);
    const uint32_t cw = SHOT_W / DW4_SIG_W, ch = SHOT_H / DW4_SIG_H;

    if (!fb)
        return 0;
    for (uint32_t cy = 0; cy < DW4_SIG_H; cy++) {
        for (uint32_t cx = 0; cx < DW4_SIG_W; cx++) {
            uint32_t sum = 0, n = 0;
            /* 1 pixel em cada 4x4: basta para uma media */
            for (uint32_t y = cy * ch; y < (cy + 1) * ch; y += 4) {
                const uint32_t *row = (const uint32_t *)(fb + (size_t)y * pitch);
                for (uint32_t x = cx * cw; x < (cx + 1) * cw; x += 4) {
                    uint32_t v = row[x];
                    sum += ((v >> 16) & 0xFF) * 3 + ((v >> 8) & 0xFF) * 6 + (v & 0xFF);
                    n += 10;
                }
            }
            sig[cy * DW4_SIG_W + cx] = (uint8_t)(sum / (n ? n : 1));
        }
    }
    return 1;
}

int dw4_screenshot_save(const char *path)
{
    uint32_t pitch = 0, row = SHOT_W * 3, filesz = 54 + row * SHOT_H;
    uint32_t w = SHOT_W, h = SHOT_H;
    const uint8_t *fb = frame_ptr(&pitch);
    uint8_t hdr[54] = { 'B', 'M' };
    uint8_t *line;
    FILE *f;

    if (!fb)
        return 0;
    line = malloc(row);
    f = fopen(path, "wb");
    if (!f || !line) {
        if (f) fclose(f);
        free(line);
        return 0;
    }
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (uint32_t y = SHOT_H; y-- > 0; ) {       /* BMP: de baixo para cima */
        const uint32_t *src = (const uint32_t *)(fb + (size_t)y * pitch);
        for (uint32_t x = 0; x < SHOT_W; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x]);
            line[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            line[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    free(line);
    return 1;
}

static void *shot_thread(void *arg)
{
    int seq = 0;

    (void)arg;
    for (;;) {
        char path[512];

        usleep((useconds_t)(s_every * 1e6));
        snprintf(path, sizeof path, "%s%03d.bmp", s_prefix, seq);
        if (dw4_screenshot_save(path)) {
            fprintf(stderr, "[SHOT] %s\n", path);
            seq++;
        }
    }
    return NULL;
}

void dw4_screenshot_init(void)
{
    pthread_t t;
    const char *every = getenv("DW4_SHOT_EVERY");

    s_prefix = getenv("DW4_SHOT");
    if (!s_prefix || !*s_prefix)
        return;
    if (every && atof(every) > 0.05)
        s_every = atof(every);
    if (pthread_create(&t, NULL, shot_thread, NULL) == 0)
        pthread_detach(t);
}
