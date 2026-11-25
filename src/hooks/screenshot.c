/**
 * Acesso ao frame exibido: captura em BMP e "assinatura" de baixa resolucao.
 *
 *   DW4_SHOT=<prefixo>      grava <prefixo>NNN.bmp periodicamente
 *   DW4_SHOT_EVERY=<seg>    intervalo (padrao 5 s)
 *
 * Fonte do frame, nesta ordem:
 *   1. o frame que o renderizador entregou direto (dw4_frame_note_host), de
 *      qualquer tamanho -- resolucao interna / widescreen (render_opts.h);
 *   2. a superficie que o back end trouxe para a memoria do guest no flip
 *      (dw4_frame_note), 640x480;
 *   3. sem back end, o endereco de scanout (PCRTC_START / AvSetDisplayMode).
 *
 * A assinatura (grade 16x12 de luminancia media) e o que o roteiro de entrada
 * usa para saber se a tela mudou (input_script.c) -- sem depender de tempo.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xbox_memory_layout.h"
#include "dw4_hooks.h"
#include "host_os.h"

#define SHOT_W 640u
#define SHOT_H 480u

static const char *s_prefix;
static double s_every = 5;

/* Ultimo frame apresentado (guest VA resolvido) e contagem de flips. */
static volatile uint32_t s_frame_va, s_frame_pitch;
static volatile uint64_t s_frame_count;

static void ring_push(void);

void dw4_frame_note(uint32_t va, uint32_t pitch)
{
    if (va) {
        s_frame_va = va;
        s_frame_pitch = pitch;
    }
    __atomic_add_fetch(&s_frame_count, 1, __ATOMIC_RELAXED);
    ring_push();
}

uint64_t dw4_frame_count(void)
{
    return __atomic_load_n(&s_frame_count, __ATOMIC_RELAXED);
}

/* Frame do renderizador (memoria dele). O mutex so garante que o buffer nao
 * e liberado no meio de uma leitura; o conteudo pode estar sendo reescrito. */
static pthread_mutex_t s_host_lock = PTHREAD_MUTEX_INITIALIZER;
static const uint8_t *s_host_px;
static uint32_t s_host_w, s_host_h, s_host_pitch;

/* Captura sob demanda: um renderizador que apresenta direto pela GPU (o
 * swapchain do Vulkan) nao traz o quadro para a memoria a cada flip -- era o
 * custo mais alto dele em resolucao alta. Ele avisa com dw4_frame_on_demand(1)
 * e, a cada quadro, pergunta dw4_frame_wanted(): se alguem quer pixels (uma
 * captura, a assinatura do roteiro, a caixa-preta), le aquele quadro e o
 * entrega por dw4_frame_note_host. Quem consome pede e espera o proximo. */
static volatile int s_on_demand;
static volatile int s_want;             /* quadros ainda a entregar */
static volatile uint64_t s_host_seq;    /* entregas de dw4_frame_note_host */
static int s_ring_on = -1;

void dw4_frame_on_demand(int on)
{
    s_on_demand = on;
}

int dw4_frame_wanted(void)
{
    if (s_ring_on > 0)
        return 1;
    if (s_want > 0) {
        __atomic_sub_fetch(&s_want, 1, __ATOMIC_RELAXED);
        return 1;
    }
    return 0;
}

/* Antes de ler o frame: garante um quadro recente (no maximo ~0,3 s de
 * espera; com o jogo parado fica o ultimo que foi entregue). */
static void want_fresh_frame(void)
{
    uint64_t seq = s_host_seq;
    struct timespec ts = { 0, 2000000 };

    if (!s_on_demand)
        return;
    __atomic_store_n(&s_want, 2, __ATOMIC_RELAXED);
    for (int i = 0; i < 150 && s_host_seq == seq; i++)
        nanosleep(&ts, NULL);
}

void dw4_frame_note_host(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t pitch)
{
    pthread_mutex_lock(&s_host_lock);
    s_host_px = (bgra && w && h) ? bgra : NULL;
    s_host_w = w; s_host_h = h; s_host_pitch = pitch;
    if (s_host_px)
        s_host_seq++;
    pthread_mutex_unlock(&s_host_lock);
}

/* Ponteiro do host para o frame exibido, ou NULL. Chamar com s_host_lock. */
static const uint8_t *frame_ptr(uint32_t *pitch, uint32_t *w, uint32_t *h)
{
    uint32_t va = s_frame_va, p = s_frame_pitch;

    if (s_host_px) {
        *pitch = s_host_pitch; *w = s_host_w; *h = s_host_h;
        return s_host_px;
    }
    *w = SHOT_W; *h = SHOT_H;

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
    uint32_t pitch = 0, fw = 0, fh = 0, cw, ch;
    const uint8_t *fb;

    want_fresh_frame();
    pthread_mutex_lock(&s_host_lock);
    fb = frame_ptr(&pitch, &fw, &fh);
    cw = fw / DW4_SIG_W; ch = fh / DW4_SIG_H;
    if (!fb || !cw || !ch) {
        pthread_mutex_unlock(&s_host_lock);
        return 0;
    }
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
    pthread_mutex_unlock(&s_host_lock);
    return 1;
}

int dw4_screenshot_save(const char *path)
{
    want_fresh_frame();
    return dw4_screenshot_save_now(path);
}

/* Sem esperar por um quadro novo: para a propria thread do renderizador. */
int dw4_screenshot_save_now(const char *path)
{
    uint32_t pitch = 0, row, filesz, w = 0, h = 0;
    const uint8_t *fb;
    uint8_t hdr[54] = { 'B', 'M' };
    uint8_t *line;
    FILE *f;

    pthread_mutex_lock(&s_host_lock);
    fb = frame_ptr(&pitch, &w, &h);
    row = (w * 3 + 3) & ~3u;                     /* linhas do BMP: multiplo de 4 */
    filesz = 54 + row * h;
    if (!fb) {
        pthread_mutex_unlock(&s_host_lock);
        return 0;
    }
    line = calloc(1, row);
    f = fopen(path, "wb");
    if (!f || !line) {
        if (f) fclose(f);
        free(line);
        pthread_mutex_unlock(&s_host_lock);
        return 0;
    }
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (uint32_t y = h; y-- > 0; ) {            /* BMP: de baixo para cima */
        const uint32_t *src = (const uint32_t *)(fb + (size_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            line[x * 3 + 0] = (uint8_t)(src[x]);
            line[x * 3 + 1] = (uint8_t)(src[x] >> 8);
            line[x * 3 + 2] = (uint8_t)(src[x] >> 16);
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    free(line);
    pthread_mutex_unlock(&s_host_lock);
    return 1;
}

/* ── Caixa-preta: os ultimos ~2 s de quadros ───────────────────────────────
 *
 * Um defeito de imagem que dura um quadro ("a tela pisca") nao da tempo de
 * apertar F9. Aqui cada quadro apresentado e guardado reduzido (ate 640 de
 * largura) num anel de RING_N quadros, e o anel e gravado em disco:
 *
 *   - sozinho, quando parte da tela muda forte de um quadro para o outro E,
 *     em ate 12 quadros, a tela volta a ser a de antes (um clarao; corte de
 *     cena e camera andando nao voltam);
 *   - a pedido: F12 na janela do jogo (dw4_frame_ring_dump).
 *
 * Saida: <DW4_SHOT_DIR ou build/shots>/flash-NNN/ com um BMP por quadro
 * (o suspeito tem SUSPEITO no nome) e info.txt com flip, diferenca para o
 * anterior e a etiqueta que o renderizador deu ao quadro (dw4_frame_tag).
 *
 *   DW4_FRAME_RING=0     desliga      DW4_FLASH_MAX=<n>  gravacoes automaticas
 *                                                        por sessao (padrao 12)
 */
#define RING_N 240
#define RING_W 640u
#define RING_HMAX 480u

typedef struct {
    uint32_t *px;
    uint32_t w, h;
    uint64_t flip;
    uint8_t sig[DW4_SIG_CELLS];
    float diff_prev;                    /* media |sig - sig do quadro anterior| */
    float dt_ms;                        /* tempo desde o flip anterior (relogio do host) */
    char tag[320];
    char *detail;                       /* um lote por linha (dw4_frame_detail) */
} RingFrame;

static RingFrame s_ring[RING_N];
static unsigned s_ring_head;            /* proximo a escrever */
static unsigned s_ring_count;
static int s_ring_busy;
static int s_flash_left = 12, s_flash_wait = -1, s_flash_slot = -1;
static int s_flash_cells = 14, s_flash_thr = 18;   /* DW4_FLASH_CELLS / DW4_FLASH_THR */
static char s_next_tag[320];
#define DETAIL_CAP 24576
static char s_next_detail[DETAIL_CAP];
static int s_next_detail_len;

/* Uma linha por lote desenhado no quadro corrente (thread do executor). */
void dw4_frame_detail(const char *line)
{
    int n = (int)strlen(line);

    if (s_ring_on > 0 && s_next_detail_len + n + 2 < DETAIL_CAP) {
        memcpy(s_next_detail + s_next_detail_len, line, (size_t)n);
        s_next_detail_len += n;
        s_next_detail[s_next_detail_len++] = '\n';
        s_next_detail[s_next_detail_len] = '\0';
    }
}
static pthread_mutex_t s_ring_lock = PTHREAD_MUTEX_INITIALIZER;

void dw4_frame_tag(const char *tag)
{
    snprintf(s_next_tag, sizeof s_next_tag, "%s", tag ? tag : "");
}

static float sig_diff(const uint8_t *a, const uint8_t *b)
{
    uint32_t sum = 0;

    for (int i = 0; i < DW4_SIG_CELLS; i++)
        sum += (uint32_t)abs((int)a[i] - (int)b[i]);
    return (float)sum / DW4_SIG_CELLS;
}

/* Quantas celulas da grade diferem mais que `thr`. */
static int sig_cells(const uint8_t *a, const uint8_t *b, int thr)
{
    int n = 0;

    for (int i = 0; i < DW4_SIG_CELLS; i++)
        n += abs((int)a[i] - (int)b[i]) > thr;
    return n;
}

static void bmp_write(const char *path, const uint32_t *px, uint32_t w, uint32_t h)
{
    uint32_t row = (w * 3 + 3) & ~3u, filesz = 54 + row * h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint8_t *line = calloc(1, row);
    FILE *f = fopen(path, "wb");

    if (f && line) {
        memcpy(hdr + 2, &filesz, 4);
        hdr[10] = 54; hdr[14] = 40;
        memcpy(hdr + 18, &w, 4);
        memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof hdr, f);
        for (uint32_t y = h; y-- > 0; ) {
            const uint32_t *src = px + (size_t)y * w;
            for (uint32_t x = 0; x < w; x++) {
                line[x * 3 + 0] = (uint8_t)(src[x]);
                line[x * 3 + 1] = (uint8_t)(src[x] >> 8);
                line[x * 3 + 2] = (uint8_t)(src[x] >> 16);
            }
            fwrite(line, 1, row, f);
        }
    }
    if (f) fclose(f);
    free(line);
}

static void *ring_dump_thread(void *arg)
{
    const char *dir = getenv("DW4_SHOT_DIR");
    int suspect = (int)(intptr_t)arg, seq = 0;
    char base[400], path[512];
    FILE *info;

    if (!dir || !*dir)
        dir = "build/shots";
    for (; seq < 1000; seq++) {
        snprintf(base, sizeof base, "%s/flash-%03d", dir, seq);
        if (access(base, F_OK) != 0)
            break;
    }
    dw4_os_mkdir(dir);
    dw4_os_mkdir(base);
    snprintf(path, sizeof path, "%s/info.txt", base);
    info = fopen(path, "w");
    for (unsigned i = 0; i < s_ring_count; i++) {
        unsigned slot = (s_ring_head + RING_N - s_ring_count + i) % RING_N;
        const RingFrame *fr = &s_ring[slot];
        int sus = (int)slot == suspect;

        if (!fr->px)
            continue;
        snprintf(path, sizeof path, "%s/%03u%s.bmp", base, i, sus ? "-SUSPEITO" : "");
        bmp_write(path, fr->px, fr->w, fr->h);
        if (info)
            fprintf(info, "%03u flip=%llu dt=%.1fms dif=%.1f %s%s\n", i,
                    (unsigned long long)fr->flip, (double)fr->dt_ms, (double)fr->diff_prev,
                    fr->tag, sus ? "  <== SUSPEITO" : "");
    }
    if (info) fclose(info);
    snprintf(path, sizeof path, "%s/lotes.txt", base);
    info = fopen(path, "w");
    for (unsigned i = 0; info && i < s_ring_count; i++) {
        unsigned slot = (s_ring_head + RING_N - s_ring_count + i) % RING_N;
        const RingFrame *fr = &s_ring[slot];

        fprintf(info, "== %03u flip=%llu%s\n%s", i, (unsigned long long)fr->flip,
                (int)slot == suspect ? " SUSPEITO" : "", fr->detail ? fr->detail : "");
    }
    if (info) fclose(info);
    fprintf(stderr, "[FLASH] %u quadros gravados em %s/%s\n", s_ring_count, base,
            suspect >= 0 ? " (clarao detectado)" : " (F12)");
    pthread_mutex_lock(&s_ring_lock);
    s_ring_busy = 0;
    pthread_mutex_unlock(&s_ring_lock);
    return NULL;
}

static void ring_dump_start(int suspect_slot)
{
    pthread_t t;

    pthread_mutex_lock(&s_ring_lock);
    if (s_ring_busy || !s_ring_count) {
        pthread_mutex_unlock(&s_ring_lock);
        return;
    }
    s_ring_busy = 1;                    /* o anel para de girar ate gravar */
    pthread_mutex_unlock(&s_ring_lock);
    if (pthread_create(&t, NULL, ring_dump_thread, (void *)(intptr_t)suspect_slot) == 0)
        pthread_detach(t);
    else
        s_ring_busy = 0;
}

void dw4_frame_ring_dump(void)
{
    if (s_ring_on > 0)
        ring_dump_start(-1);
    else
        fprintf(stderr, "[FLASH] a caixa-preta de quadros esta desligada "
                        "(ligue com DW4_FRAME_RING=1)\n");
}

int dw4_frame_ring_on(void)
{
    return s_ring_on > 0;
}

/* A cada flip, na thread do executor. */
static void ring_push(void)
{
    uint32_t pitch = 0, w = 0, h = 0, rw, rh;
    const uint8_t *fb;
    RingFrame *fr, *prev;

    if (s_ring_on < 0) {
        const char *e = getenv("DW4_FRAME_RING"), *m = getenv("DW4_FLASH_MAX");
        /* Desligada por padrao desde a DW4R-2: custa uma copia reduzida de
         * cada quadro e uma linha de texto por lote, e obriga o renderizador
         * a trazer todo quadro da GPU. E ferramenta de investigacao. */
        s_ring_on = e && e[0] && e[0] != '0';
        if (s_ring_on)
            fprintf(stderr, "[FLASH] caixa-preta de quadros ligada (DW4_FRAME_RING)\n");
        if (m)
            s_flash_left = atoi(m);
        if (getenv("DW4_FLASH_CELLS"))
            s_flash_cells = atoi(getenv("DW4_FLASH_CELLS"));
        if (getenv("DW4_FLASH_THR"))
            s_flash_thr = atoi(getenv("DW4_FLASH_THR"));
    }
    if (!s_ring_on || s_ring_busy)
        return;

    pthread_mutex_lock(&s_host_lock);
    fb = frame_ptr(&pitch, &w, &h);
    if (!fb || w < 64 || h < 64) {
        pthread_mutex_unlock(&s_host_lock);
        return;
    }
    rw = w < RING_W ? w : RING_W;
    rh = (uint32_t)((uint64_t)h * rw / w);
    if (rh > RING_HMAX) rh = RING_HMAX;
    fr = &s_ring[s_ring_head];
    if (!fr->px || fr->w != rw || fr->h != rh) {
        free(fr->px);
        fr->px = malloc((size_t)rw * rh * 4);
        fr->w = rw; fr->h = rh;
    }
    if (fr->px) {
        uint32_t cell[DW4_SIG_CELLS] = { 0 }, cnt[DW4_SIG_CELLS] = { 0 };

        for (uint32_t y = 0; y < rh; y++) {
            const uint32_t *src = (const uint32_t *)(fb + (size_t)(y * h / rh) * pitch);
            uint32_t *dst = fr->px + (size_t)y * rw;
            uint32_t cy = y * DW4_SIG_H / rh;

            for (uint32_t x = 0; x < rw; x++) {
                uint32_t v = src[(uint64_t)x * w / rw];
                dst[x] = v;
                if (!((x | y) & 3)) {
                    uint32_t c = cy * DW4_SIG_W + x * DW4_SIG_W / rw;
                    cell[c] += (((v >> 16) & 0xFF) * 3 + ((v >> 8) & 0xFF) * 6 + (v & 0xFF)) / 10;
                    cnt[c]++;
                }
            }
        }
        for (int i = 0; i < DW4_SIG_CELLS; i++)
            fr->sig[i] = (uint8_t)(cell[i] / (cnt[i] ? cnt[i] : 1));
    }
    pthread_mutex_unlock(&s_host_lock);
    if (!fr->px)
        return;

    {
        static struct timespec last;
        struct timespec now;

        clock_gettime(CLOCK_MONOTONIC, &now);
        fr->dt_ms = last.tv_sec ? (float)((now.tv_sec - last.tv_sec) * 1e3
                                          + (now.tv_nsec - last.tv_nsec) / 1e6) : 0.0f;
        last = now;
    }
    fr->flip = dw4_frame_count();
    memcpy(fr->tag, s_next_tag, sizeof fr->tag);
    if (!fr->detail)
        fr->detail = malloc(DETAIL_CAP);
    if (fr->detail)
        memcpy(fr->detail, s_next_detail, (size_t)s_next_detail_len + 1);
    s_next_detail_len = 0;
    s_next_detail[0] = '\0';
    prev = &s_ring[(s_ring_head + RING_N - 1) % RING_N];
    fr->diff_prev = (s_ring_count && prev->px) ? sig_diff(fr->sig, prev->sig) : 0.0f;

    /* Clarao: o quadro N muda forte em parte da tela em relacao ao N-1 e, em
     * ate 3 quadros, a tela volta a ser a do N-1. Conta por celula da grade
     * (e nao a media do quadro): um chao escuro que vai a preto mexe pouco na
     * media, mas mexe em muitas celulas. Corte de cena e camera andando nao
     * voltam ao quadro de referencia. Grava meio segundo depois, para pegar a
     * sequencia; no minimo 4 s entre gravacoes. */
    {
        static uint8_t ref[DW4_SIG_CELLS];
        static int cand_slot = -1, cand_strong, cand_ttl;
        static uint64_t last_dump_flip;

        if (cand_slot >= 0) {
            int back = sig_cells(fr->sig, ref, s_flash_thr * 2 / 3);

            if (back <= (cand_strong / 5 > 2 ? cand_strong / 5 : 2)) {
                const RingFrame *c = &s_ring[cand_slot];

                s_flash_slot = cand_slot;
                s_flash_wait = 30;
                s_flash_left--;
                last_dump_flip = fr->flip;
                fprintf(stderr, "[FLASH] flip %llu: %d de %d celulas mudaram e a tela voltou "
                        "em %d quadro(s) (dif media %.1f, dt %.1f ms, anterior %.1f ms) %s\n",
                        (unsigned long long)c->flip, cand_strong, DW4_SIG_CELLS, 13 - cand_ttl,
                        (double)c->diff_prev, (double)c->dt_ms,
                        (double)s_ring[(cand_slot + RING_N - 1) % RING_N].dt_ms, c->tag);
                cand_slot = -1;
            } else if (--cand_ttl <= 0) {
                cand_slot = -1;
            }
        } else if (s_ring_count >= 1 && prev->px && s_flash_left > 0 && s_flash_wait < 0
                   && fr->flip > last_dump_flip + 240) {
            int strong = sig_cells(fr->sig, prev->sig, s_flash_thr);

            if (strong >= s_flash_cells) {      /* padrao: 14 celulas, ~7% da tela */
                memcpy(ref, prev->sig, sizeof ref);
                cand_slot = (int)s_ring_head;
                cand_strong = strong;
                cand_ttl = 12;                  /* o chao escuro durou ~7 quadros */
            }
        }
    }
    s_ring_head = (s_ring_head + 1) % RING_N;
    if (s_ring_count < RING_N)
        s_ring_count++;
    if (s_flash_wait >= 0 && s_flash_wait-- == 0) {
        s_flash_wait = -1;
        ring_dump_start(s_flash_slot);
    }
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
