/**
 * Roteiro de entrada para rodar o jogo sem ninguem no controle.
 *
 *   DW4_INPUT_SCRIPT="sleep:17 enter! enter! z! shot:lobby quit"
 *   DW4_INPUT_SCRIPT=@tests/scenarios/lobby.txt        (arquivo; '#' comenta)
 *
 * Passos, executados em ordem:
 *
 *   f<N>:<tecla>[*seg] toque quando o jogo chegar ao flip N (frame apresentado
 *                      N desde o boot). FORMA RECOMENDADA: a logica do jogo
 *                      anda por frame, entao nao depende da velocidade da maquina.
 *   <tecla>            toque de 0,15 s agora
 *   <tecla>*<seg>      segura por <seg> segundos
 *   <tecla>!           EXPERIMENTAL: aperta e espera a TELA MUDAR; repete (ate
 *                      8 vezes) se nada mudar. Nao e confiavel neste jogo: os
 *                      menus tem fundo animado, o passo espera ate 30 s a tela
 *                      "acalmar" e as vezes se perde. Use f<N>: nos cenarios.
 *   sleep:<seg>        espera
 *   wait_change:<seg>  espera a tela mudar sem apertar nada (no maximo <seg>)
 *   wait_flips:<n>     espera <n> flips (frames apresentados)
 *   shot:<nome>        grava <DW4_SHOT_DIR>/<nome>.bmp (padrao build/shots)
 *   mark:<texto>       linha "[SCRIPT] mark <texto>" no log
 *   vpad / vpad_off    conecta / desconecta um controle virtual (teste de
 *                      plugar o controle com o jogo aberto, sem hardware)
 *   quit               encerra o processo com codigo 0 (run automatizado OK)
 *   <t>:<tecla>[*seg]  forma antiga: toque no instante absoluto <t> s do boot
 *
 * Teclas (mesma tabela do RECOMP_KEYBOARD=1):
 *   enter=START  back=BACK  z=A  x=B  c=X  v=Y  q=White  e=Black
 *   up/down/left/right=D-pad  w/a/s/d=analogico esq.  i/j/k/l=analogico dir.
 *   1=gatilho esq.  3=gatilho dir.
 *
 * "A tela mudou": compara a assinatura 16x12 do frame (screenshot.c). Antes
 * de apertar mede-se por ~0,8 s o ruido natural de cada celula (fundos
 * animados). Se mais de 20% das celulas estao agitadas (transicao, video), o
 * passo espera a tela acalmar. Depois do toque, uma celula conta como mudada
 * se passou de 3x o seu ruido + 10, e a tela mudou se pelo menos 3 celulas
 * mudaram em duas leituras seguidas. Videos nunca acalmam: para pular um FMV
 * use a tecla simples + sleep.
 */

#include <ctype.h>
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "keyboard.h"
#include "dw4_hooks.h"

static const struct { const char *name; int vk; } s_keys[] = {
    { "enter", XBOX_VK_RETURN }, { "start", XBOX_VK_RETURN },
    { "back", XBOX_VK_BACK },
    { "up", XBOX_VK_UP }, { "down", XBOX_VK_DOWN },
    { "left", XBOX_VK_LEFT }, { "right", XBOX_VK_RIGHT },
    { "shift", XBOX_VK_SHIFT }, { "ctrl", XBOX_VK_CONTROL },
};

static int key_vk(const char *name)
{
    for (size_t i = 0; i < sizeof s_keys / sizeof s_keys[0]; i++)
        if (strcasecmp(name, s_keys[i].name) == 0)
            return s_keys[i].vk;
    if (name[0] && !name[1] && isalnum((unsigned char)name[0]))
        return toupper((unsigned char)name[0]);   /* VK de letra/digito = ASCII */
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void sleep_s(double d)
{
    if (d > 0) {
        struct timespec ts = { (time_t)d, (long)((d - (double)(time_t)d) * 1e9) };
        nanosleep(&ts, NULL);
    }
}

static void press(int vk, double hold)
{
    xbox_InputKeySet(vk, 1);
    sleep_s(hold);
    xbox_InputKeySet(vk, 0);
}

/* ── Deteccao de mudanca de tela ───────────────────────────────────────────── */

typedef struct {
    uint8_t base[DW4_SIG_CELLS];
    uint8_t noise[DW4_SIG_CELLS];
    int ok;
} Baseline;

/* Tira a referencia e mede o ruido natural de cada celula por ~0,8 s.
 * `noisy`: quantas celulas variaram mais de 12 sozinhas -- se for grande, a
 * tela esta em transicao ou tocando video e nao e hora de apertar nada. */
static int baseline_take(Baseline *b)
{
    uint8_t s[DW4_SIG_CELLS];
    int noisy = 0;

    memset(b, 0, sizeof *b);
    b->ok = dw4_frame_signature(b->base);
    for (int i = 0; i < 5 && b->ok; i++) {
        sleep_s(0.16);
        if (!dw4_frame_signature(s))
            continue;
        for (int c = 0; c < DW4_SIG_CELLS; c++) {
            int d = abs((int)s[c] - (int)b->base[c]);
            if (d > b->noise[c])
                b->noise[c] = (uint8_t)d;
        }
    }
    for (int c = 0; c < DW4_SIG_CELLS; c++)
        noisy += b->noise[c] > 12;
    /* referencia = o frame mais recente, nao o de 0,8 s atras */
    if (b->ok)
        dw4_frame_signature(b->base);
    return noisy;
}

#define CALM_MAX (DW4_SIG_CELLS / 5)    /* ate 20% de celulas agitadas = tela calma */

static int cells_changed(const Baseline *b)
{
    uint8_t s[DW4_SIG_CELLS];
    int n = 0;

    if (!b->ok || !dw4_frame_signature(s))
        return 0;
    for (int c = 0; c < DW4_SIG_CELLS; c++)
        if (abs((int)s[c] - (int)b->base[c]) > 3 * (int)b->noise[c] + 10)
            n++;
    return n;
}

/* Espera a tela mudar em relacao a `b`, no maximo `timeout` s. */
static int wait_changed(const Baseline *b, double timeout)
{
    double end = now_s() + timeout;
    int streak = 0;

    while (now_s() < end) {
        sleep_s(0.1);
        if (cells_changed(b) >= 3) {
            if (++streak >= 2)
                return 1;
        } else {
            streak = 0;
        }
    }
    return 0;
}

/* ── Execucao ──────────────────────────────────────────────────────────────── */

static void run_step(char *tok, double t0)
{
    char *colon = strchr(tok, ':');
    size_t len = strlen(tok);

    if (strcasecmp(tok, "vpad") == 0 || strcasecmp(tok, "vpad_off") == 0) {
        /* Conecta / desconecta um controle virtual da SDL (Xbox 360): o mesmo
         * caminho de plugar um controle com o jogo aberto, sem hardware. */
        static int vpad_index = -1;

        if (!SDL_WasInit(SDL_INIT_JOYSTICK))
            SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);
        if (tok[4] == '\0' && vpad_index < 0) {
            SDL_VirtualJoystickDesc d;

            SDL_zero(d);
            d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
            d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
            d.naxes = SDL_CONTROLLER_AXIS_MAX;
            d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
            d.vendor_id = 0x045E;
            d.product_id = 0x028E;
            d.name = "DW4 virtual pad";
            vpad_index = SDL_JoystickAttachVirtualEx(&d);
            fprintf(stderr, "[SCRIPT] vpad: controle virtual %s (t=%.1fs)\n",
                    vpad_index < 0 ? SDL_GetError() : "conectado", now_s() - t0);
        } else if (tok[4] != '\0' && vpad_index >= 0) {
            SDL_JoystickDetachVirtual(vpad_index);
            vpad_index = -1;
            fprintf(stderr, "[SCRIPT] vpad_off: controle virtual desconectado (t=%.1fs)\n",
                    now_s() - t0);
        }
        return;
    }
    if (strcasecmp(tok, "quit") == 0) {
        fprintf(stderr, "[SCRIPT] quit (t=%.1fs, %llu flips)\n", now_s() - t0,
                (unsigned long long)dw4_frame_count());
        fflush(NULL);
        _exit(0);
    }
    if (colon && (tok[0] == 'f' || tok[0] == 'F') && isdigit((unsigned char)tok[1])) {
        /* f<N>:<tecla>[*seg] -- no flip N desde o boot. A logica do jogo anda
         * por frame (60 Hz), entao isto e deterministico mesmo se a maquina
         * engasgar; um tempo em segundos nao e. */
        uint64_t target = (uint64_t)atoll(tok + 1);
        double hold = 0.15, end = now_s() + 600.0;
        char *star = strchr(colon + 1, '*');
        int vk;

        *colon++ = '\0';
        if (star) { *star++ = '\0'; hold = atof(star); }
        vk = key_vk(colon);
        if (!vk) {
            fprintf(stderr, "[SCRIPT] tecla desconhecida: %s\n", colon);
            return;
        }
        while (dw4_frame_count() < target && now_s() < end)
            sleep_s(0.004);
        fprintf(stderr, "[SCRIPT] f%llu %s (t=%.1fs)\n",
                (unsigned long long)dw4_frame_count(), colon, now_s() - t0);
        press(vk, hold);
        return;
    }
    if (colon && isalpha((unsigned char)tok[0])) {     /* comando:arg */
        *colon++ = '\0';
        if (strcasecmp(tok, "sleep") == 0) {
            sleep_s(atof(colon));
        } else if (strcasecmp(tok, "wait_change") == 0) {
            Baseline b;
            (void)baseline_take(&b);
            fprintf(stderr, "[SCRIPT] wait_change %s (t=%.1fs)\n",
                    wait_changed(&b, atof(colon)) ? "ok" : "TEMPO ESGOTADO",
                    now_s() - t0);
        } else if (strcasecmp(tok, "wait_flips") == 0) {
            uint64_t target = dw4_frame_count() + (uint64_t)atoll(colon);
            double end = now_s() + 120.0;
            while (dw4_frame_count() < target && now_s() < end)
                sleep_s(0.01);
        } else if (strcasecmp(tok, "shot") == 0) {
            const char *dir = getenv("DW4_SHOT_DIR");
            char path[512];
            snprintf(path, sizeof path, "%s/%s.bmp", dir && *dir ? dir : "build/shots",
                     colon);
            fprintf(stderr, "[SCRIPT] shot %s %s\n", path,
                    dw4_screenshot_save(path) ? "ok" : "FALHOU");
        } else if (strcasecmp(tok, "mark") == 0) {
            fprintf(stderr, "[SCRIPT] mark %s (t=%.1fs, %llu flips)\n", colon,
                    now_s() - t0, (unsigned long long)dw4_frame_count());
        } else {
            fprintf(stderr, "[SCRIPT] comando desconhecido: %s\n", tok);
        }
        return;
    }
    if (colon) {                                       /* <t>:<tecla>[*seg] */
        double at = atof(tok), hold = 0.15;
        char *star = strchr(colon + 1, '*');
        int vk;

        *colon++ = '\0';
        if (star) { *star++ = '\0'; hold = atof(star); }
        vk = key_vk(colon);
        if (!vk) {
            fprintf(stderr, "[SCRIPT] tecla desconhecida: %s\n", colon);
            return;
        }
        sleep_s(t0 + at - now_s());
        fprintf(stderr, "[SCRIPT] t=%.1fs %s\n", at, colon);
        press(vk, hold);
        return;
    }
    if (len > 1 && tok[len - 1] == '!') {              /* tecla! */
        int vk;

        tok[len - 1] = '\0';
        vk = key_vk(tok);
        if (!vk) {
            fprintf(stderr, "[SCRIPT] tecla desconhecida: %s\n", tok);
            return;
        }
        for (int attempt = 1; attempt <= 8; attempt++) {
            Baseline b;
            int noisy = baseline_take(&b), waited = 0;

            /* Espera a tela acalmar (transicao, video) antes de apertar:
             * ate 30 s. Apertar no meio de uma animacao costuma ser ignorado
             * pelo jogo e nao ha como medir o efeito. */
            while (noisy > CALM_MAX && waited < 37) {
                noisy = baseline_take(&b);
                waited++;
            }
            press(vk, 0.15);
            if (wait_changed(&b, 2.5)) {
                fprintf(stderr, "[SCRIPT] %s! ok na tentativa %d (t=%.1fs, agitacao %d/%d"
                        "%s)\n", tok, attempt, now_s() - t0, noisy, DW4_SIG_CELLS,
                        noisy > CALM_MAX ? ", TELA NAO ACALMOU" : "");
                sleep_s(0.5);               /* deixa a transicao assentar */
                return;
            }
            fprintf(stderr, "[SCRIPT] %s! tentativa %d sem mudanca (t=%.1fs, agitacao %d)\n",
                    tok, attempt, now_s() - t0, noisy);
        }
        fprintf(stderr, "[SCRIPT] %s! SEM EFEITO apos 8 tentativas (t=%.1fs)\n", tok,
                now_s() - t0);
        return;
    }
    {                                                  /* tecla[*seg] */
        double hold = 0.15;
        char *star = strchr(tok, '*');
        int vk;

        if (star) { *star++ = '\0'; hold = atof(star); }
        vk = key_vk(tok);
        if (!vk) {
            fprintf(stderr, "[SCRIPT] passo desconhecido: %s\n", tok);
            return;
        }
        fprintf(stderr, "[SCRIPT] %s (t=%.1fs)\n", tok, now_s() - t0);
        press(vk, hold);
    }
}

static void *script_thread(void *arg)
{
    char *script = arg, *save = NULL;
    double t0 = now_s();

    for (char *tok = strtok_r(script, " \t\r\n,;", &save); tok;
         tok = strtok_r(NULL, " \t\r\n,;", &save))
        run_step(tok, t0);
    fprintf(stderr, "[SCRIPT] fim do roteiro (t=%.1fs)\n", now_s() - t0);
    free(script);
    return NULL;
}

/* Le um arquivo de cenario, descartando comentarios (# ate o fim da linha). */
static char *load_scenario(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024], *out = NULL;
    size_t len = 0;

    if (!f) {
        fprintf(stderr, "[SCRIPT] nao abriu %s\n", path);
        return NULL;
    }
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        size_t n;
        if (hash)
            *hash = '\0';
        n = strlen(line);
        out = realloc(out, len + n + 2);
        memcpy(out + len, line, n);
        len += n;
        out[len++] = ' ';
        out[len] = '\0';
    }
    fclose(f);
    return out;
}

void dw4_input_script_init(void)
{
    const char *s = getenv("DW4_INPUT_SCRIPT");
    char *script;
    pthread_t t;

    if (!s || !*s)
        return;
    script = s[0] == '@' ? load_scenario(s + 1) : strdup(s);
    if (script && pthread_create(&t, NULL, script_thread, script) == 0)
        pthread_detach(t);
}
