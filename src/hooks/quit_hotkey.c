/**
 * Atalhos do port (teclado e controle): fechar o jogo e ligar/desligar o
 * mostrador de desempenho (osd.c).
 *
 * Em tela cheia nao ha botao de fechar. Uma thread propria olha, a cada 20 ms,
 * as teclas da janela e os botoes dos controles; com o atalho completo ela
 * encerra o processo (ver quit_now). Por ser uma thread a parte, funciona
 * tambem com o jogo travado.
 *
 *   DW4_QUIT_KEYS=ctrl+q       teclas, todas ao mesmo tempo (ate 4); fecha na hora
 *   DW4_QUIT_PAD=back+start    botoes de um mesmo controle (2 a 4), SEGURADOS
 *   DW4_QUIT_HOLD_MS=1000      por quanto tempo segurar os botoes do controle
 *   "off" (ou vazio) desliga aquele atalho. Sem a variavel valem os padroes acima.
 *
 * Teclas: letras e digitos, f1..f12, esc, tab, space, enter, backspace, shift,
 * ctrl, alt, up, down, left, right, home, end, pgup, pgdn, ins, del, pause.
 * Botoes: a b x y back start lb rb lt rt l3 r3 up down left right.
 *
 * O controle e lido direto (xbox_InputPadHeld, patch 4.17 no toolkit): o
 * teclado fazendo as vezes de controle 1 nao conta como controle aqui, e um
 * botao sozinho nao e aceito (seria facil demais fechar sem querer).
 *
 * Mostrador de FPS / CPU / GPU, com as mesmas regras de nomes:
 *   DW4_OSD_KEYS=f2            liga/desliga ao apertar
 *   DW4_OSD_PAD=l3+r3          idem, ao completar a combinacao (sem segurar)
 *
 * O launcher grava as variaveis (Controles -> Fechar o jogo / Mostrador de
 * desempenho).
 */

#include <ctype.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "keyboard.h"
#include "xinput_xbox.h"
#include "dw4_hooks.h"

#define MAX_COMBO 4

static const struct { const char *name; int vk; } s_key_names[] = {
    { "esc", XBOX_VK_ESCAPE }, { "escape", XBOX_VK_ESCAPE },
    { "tab", XBOX_VK_TAB }, { "space", XBOX_VK_SPACE },
    { "enter", XBOX_VK_RETURN }, { "return", XBOX_VK_RETURN }, { "start", XBOX_VK_RETURN },
    { "backspace", XBOX_VK_BACK }, { "back", XBOX_VK_BACK },
    { "shift", XBOX_VK_SHIFT }, { "ctrl", XBOX_VK_CONTROL }, { "alt", XBOX_VK_MENU },
    { "up", XBOX_VK_UP }, { "down", XBOX_VK_DOWN },
    { "left", XBOX_VK_LEFT }, { "right", XBOX_VK_RIGHT },
    { "home", XBOX_VK_HOME }, { "end", XBOX_VK_END },
    { "pgup", XBOX_VK_PRIOR }, { "pgdn", XBOX_VK_NEXT },
    { "ins", XBOX_VK_INSERT }, { "del", XBOX_VK_DELETE }, { "pause", XBOX_VK_PAUSE },
};

static const struct { const char *name; unsigned bit; } s_pad_names[] = {
    { "a", 1u << SDL_CONTROLLER_BUTTON_A }, { "b", 1u << SDL_CONTROLLER_BUTTON_B },
    { "x", 1u << SDL_CONTROLLER_BUTTON_X }, { "y", 1u << SDL_CONTROLLER_BUTTON_Y },
    { "back", 1u << SDL_CONTROLLER_BUTTON_BACK }, { "start", 1u << SDL_CONTROLLER_BUTTON_START },
    { "lb", 1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
    { "rb", 1u << SDL_CONTROLLER_BUTTON_RIGHTSHOULDER },
    { "lt", 1u << 30 }, { "rt", 1u << 31 },
    { "l3", 1u << SDL_CONTROLLER_BUTTON_LEFTSTICK }, { "r3", 1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK },
    { "up", 1u << SDL_CONTROLLER_BUTTON_DPAD_UP }, { "down", 1u << SDL_CONTROLLER_BUTTON_DPAD_DOWN },
    { "left", 1u << SDL_CONTROLLER_BUTTON_DPAD_LEFT },
    { "right", 1u << SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
};

static int s_keys[MAX_COMBO], s_nkeys;
static unsigned s_pad_mask;
static unsigned s_hold_ms = 1000;
static int s_osd_keys[MAX_COMBO], s_osd_nkeys;     /* mostrador de desempenho */
static unsigned s_osd_pad_mask;

int dw4_key_vk(const char *name)
{
    if (!name || !*name)
        return 0;
    for (size_t i = 0; i < sizeof s_key_names / sizeof s_key_names[0]; i++)
        if (SDL_strcasecmp(name, s_key_names[i].name) == 0)
            return s_key_names[i].vk;
    if (!name[1] && isalnum((unsigned char)name[0]))
        return toupper((unsigned char)name[0]);     /* VK de letra/digito = ASCII */
    if ((name[0] == 'f' || name[0] == 'F') && isdigit((unsigned char)name[1])) {
        int n = atoi(name + 1);
        if (n >= 1 && n <= 12 && strlen(name) <= 3)
            return XBOX_VK_F1 + n - 1;
    }
    return 0;
}

unsigned dw4_pad_button_bit(const char *name)
{
    for (size_t i = 0; name && i < sizeof s_pad_names / sizeof s_pad_names[0]; i++)
        if (SDL_strcasecmp(name, s_pad_names[i].name) == 0)
            return s_pad_names[i].bit;
    return 0;
}

static int is_off(const char *s)
{
    return !*s || !SDL_strcasecmp(s, "off") || !SDL_strcasecmp(s, "none") || !strcmp(s, "0");
}

/* "ctrl+q" -> keys[], *count. 0 se alguma parte nao for uma tecla conhecida. */
static int parse_keys(const char *spec, int *keys, int *count)
{
    char buf[128], *save = NULL;
    int n = 0;

    SDL_strlcpy(buf, spec, sizeof buf);
    for (char *t = strtok_r(buf, "+ ", &save); t; t = strtok_r(NULL, "+ ", &save)) {
        int vk = dw4_key_vk(t);
        if (!vk || n == MAX_COMBO)
            return 0;
        keys[n++] = vk;
    }
    *count = n;
    return n > 0;
}

/* "back+start" -> *out. 0 se houver nome desconhecido, menos de 2 ou mais de 4. */
static int parse_pad(const char *spec, unsigned *out)
{
    char buf[128], *save = NULL;
    unsigned mask = 0;
    int n = 0;

    SDL_strlcpy(buf, spec, sizeof buf);
    for (char *t = strtok_r(buf, "+ ", &save); t; t = strtok_r(NULL, "+ ", &save)) {
        unsigned bit = dw4_pad_button_bit(t);
        if (!bit)
            return 0;
        if (!(mask & bit))
            n++;
        mask |= bit;
    }
    if (n < 2 || n > MAX_COMBO)
        return 0;
    *out = mask;
    return 1;
}

/* Sai direto, como o "quit" dos roteiros (input_script.c): descarrega o que
 * esta em buffer e encerra sem rodar os destrutores. Pedir o fechamento pela
 * janela (SDL_QUIT -> exit()) derruba as bibliotecas com a thread do
 * renderizador ainda apresentando: num gamescope sem tela isso caiu dentro da
 * camada de swapchain e o log terminava em [CRASH] (2026-11-25). A tela cheia
 * e sem borda, na resolucao da area de trabalho: nao ha modo de video a
 * restaurar. O jogo grava os saves direto no arquivo, na hora em que salva. */
static void quit_now(const char *how)
{
    fprintf(stderr, "[QUIT] atalho de saida (%s): fechando o jogo\n", how);
    dw4_prof_dump();
    fflush(NULL);
    _exit(0);
}

static int quit_thread(void *arg)
{
    Uint32 pad_since[XBOX_MAX_CONTROLLERS] = { 0 };
    int osd_key_was = 0, osd_pad_was[XBOX_MAX_CONTROLLERS] = { 0 };

    (void)arg;
    for (;;) {
        SDL_Delay(20);
        dw4_osd_sample();
        if (s_osd_nkeys) {
            int all = 1;

            for (int i = 0; i < s_osd_nkeys && all; i++)
                all = xbox_FramebufferKeyDown(s_osd_keys[i]);
            if (all && !osd_key_was)
                dw4_osd_toggle();
            osd_key_was = all;
        }
        if (s_nkeys) {
            int all = 1;

            for (int i = 0; i < s_nkeys && all; i++)
                all = xbox_FramebufferKeyDown(s_keys[i]);
            if (all)
                quit_now("teclado");
        }
        if (!s_pad_mask && !s_osd_pad_mask)
            continue;
        for (DWORD port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
            Uint32 now = SDL_GetTicks();
            unsigned held = xbox_InputPadHeld(port);

            if (s_osd_pad_mask) {
                int all = (held & s_osd_pad_mask) == s_osd_pad_mask;

                if (all && !osd_pad_was[port])
                    dw4_osd_toggle();
                osd_pad_was[port] = all;
            }
            if (!s_pad_mask || (held & s_pad_mask) != s_pad_mask) {
                pad_since[port] = 0;
                continue;
            }
            if (!pad_since[port])
                pad_since[port] = now ? now : 1;
            else if (now - pad_since[port] >= s_hold_ms) {
                char how[32];

                snprintf(how, sizeof how, "controle %u", (unsigned)port + 1);
                quit_now(how);
            }
        }
    }
    return 0;
}

void dw4_quit_hotkey_init(void)
{
    const char *keys = getenv("DW4_QUIT_KEYS"), *pad = getenv("DW4_QUIT_PAD");
    const char *hold = getenv("DW4_QUIT_HOLD_MS");
    const char *okeys = getenv("DW4_OSD_KEYS"), *opad = getenv("DW4_OSD_PAD");
    SDL_Thread *t;

    if (!keys)
        keys = "ctrl+q";
    if (!pad)
        pad = "back+start";
    if (hold && atoi(hold) >= 0)
        s_hold_ms = (unsigned)atoi(hold);
    if (!okeys)
        okeys = "f2";
    if (!opad)
        opad = "l3+r3";
    if (!is_off(okeys) && !parse_keys(okeys, s_osd_keys, &s_osd_nkeys)) {
        fprintf(stderr, "[OSD] DW4_OSD_KEYS=\"%s\" nao entendido: tecla do mostrador desligada\n", okeys);
        s_osd_nkeys = 0;
    }
    if (!is_off(opad) && !parse_pad(opad, &s_osd_pad_mask)) {
        fprintf(stderr, "[OSD] DW4_OSD_PAD=\"%s\" nao entendido (2 a 4 botoes): "
                        "botoes do mostrador desligados\n", opad);
        s_osd_pad_mask = 0;
    }
    if (!is_off(keys) && !parse_keys(keys, s_keys, &s_nkeys)) {
        fprintf(stderr, "[QUIT] DW4_QUIT_KEYS=\"%s\" nao entendido: atalho de teclado desligado\n", keys);
        s_nkeys = 0;
    }
    if (!is_off(pad) && !parse_pad(pad, &s_pad_mask)) {
        fprintf(stderr, "[QUIT] DW4_QUIT_PAD=\"%s\" nao entendido (2 a 4 botoes): "
                        "atalho de controle desligado\n", pad);
        s_pad_mask = 0;
    }
    if (!s_nkeys && !s_pad_mask && !s_osd_nkeys && !s_osd_pad_mask && !dw4_osd_visible())
        return;
    fprintf(stderr, "[QUIT] atalho de saida: teclado=%s controle=%s (segurar %u ms)\n",
            s_nkeys ? keys : "desligado", s_pad_mask ? pad : "desligado", s_hold_ms);
    fprintf(stderr, "[OSD] mostrador de desempenho: teclado=%s controle=%s%s\n",
            s_osd_nkeys ? okeys : "desligado", s_osd_pad_mask ? opad : "desligado",
            dw4_osd_visible() ? " (ja visivel)" : "");
    t = SDL_CreateThread(quit_thread, "dw4-quit", NULL);
    if (t)
        SDL_DetachThread(t);
}
