/*
 * Teste de plugar/desplugar controle com o "jogo" rodando, sem jogo e sem
 * hardware: um controle virtual da SDL e conectado e removido centenas de
 * vezes enquanto outra thread le o controle sem parar, como a thread do USB
 * emulado faz (usb_gamepad_report -> xbox_InputGetState).
 *
 * Reproduz o crash de 2026-11-24 ("malloc(): unaligned tcache chunk
 * detected" ao conectar o controle): antes da correcao as duas threads
 * refaziam a lista de joysticks do sdl2-compat ao mesmo tempo.
 * Mesma ideia do tests/pad_virtual.c do Amped 2 / SSX 3 (docs/REFERENCIAS.md).
 *
 *   scripts/test_pad.sh [ciclos]
 */
#include <SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xinput_xbox.h"

static SDL_atomic_t s_stop;
static unsigned long s_reads;

static void *reader(void *arg)
{
    XBOX_INPUT_STATE st;

    (void)arg;
    while (!SDL_AtomicGet(&s_stop)) {
        for (DWORD port = 0; port < 2; port++)
            xbox_InputGetState(port, &st);
        s_reads++;
    }
    return NULL;
}

static void pump(int ms)
{
    Uint32 end = SDL_GetTicks() + (Uint32)ms;
    SDL_Event ev;

    do {
        while (SDL_PollEvent(&ev)) {
#ifndef PAD_TEST_OLD
            xbox_InputSdlDeviceEvent(&ev);
#endif
        }
        SDL_Delay(1);
    } while (SDL_GetTicks() < end);
}

int main(int argc, char **argv)
{
    int cycles = argc > 1 ? atoi(argv[1]) : 300, fails = 0, seen = 0;
    SDL_VirtualJoystickDesc d;
    pthread_t th;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    xbox_InputSetMainPump(1);           /* esta thread bombeia, como a da janela */
#ifndef PAD_TEST_OLD
    xbox_InputSdlDeviceEvent(NULL);
#endif
    pthread_create(&th, NULL, reader, NULL);

    SDL_zero(d);
    d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    d.naxes = SDL_CONTROLLER_AXIS_MAX;
    d.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    d.vendor_id = 0x045E;
    d.product_id = 0x028E;
    d.name = "DW4 virtual pad";

    for (int i = 0; i < cycles; i++) {
        XBOX_INPUT_STATE st;
        int idx = SDL_JoystickAttachVirtualEx(&d);

        if (idx < 0) {
            fprintf(stderr, "attach: %s\n", SDL_GetError());
            return 2;
        }
        pump(8);
        /* o controle virtual ocupa a primeira porta livre: 1 ou 2 (se ja
         * houver um controle de verdade ligado) */
        if (xbox_InputGetState(0, &st) == 0 || xbox_InputGetState(1, &st) == 0)
            seen++;
        SDL_JoystickDetachVirtual(idx);
        pump(4);
    }
    SDL_AtomicSet(&s_stop, 1);
    pthread_join(th, NULL);
#ifndef PAD_TEST_OLD
    if (seen < cycles) {
        fprintf(stderr, "FAIL: controle visto em %d de %d conexoes\n", seen, cycles);
        fails++;
    }
#endif
    printf("%s: %d ciclos de plugar/desplugar, controle visto %d vezes, %lu leituras "
           "concorrentes\n", fails ? "FALHOU" : "ok", cycles, seen, s_reads);
    return fails != 0;
}
