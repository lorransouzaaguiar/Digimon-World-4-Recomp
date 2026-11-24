/**
 * Semaforo de progresso da GPU para o D3D8 do XDK 5849 (linkado no jogo).
 *
 * D3D_BlockOnTime (guest 0x002B1500) espera, girando em 0x002B1637, ate que
 *
 *     *(*(device + 0x30))  alcance o fence pedido   (device = MEM32(0x002BAD98))
 *
 * Esse semaforo e escrito pela GPU quando ela executa o metodo
 * BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) do pushbuffer. O executor de
 * pushbuffer do xboxrecomp faz essa escrita, mas precisa saber ONDE: o
 * endereco vem do proprio device, que so existe depois de CreateDevice.
 *
 * Em vez de editar o codigo gerado, uma thread do host observa o ponteiro do
 * device e informa o executor (nv2a_pb_set_semaphore_target) assim que ele
 * aparece -- e de novo se o device for recriado.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "xbox_memory_layout.h"
#include "nv2a_backend.h"
#include "dw4_hooks.h"

#define DW4_D3D_DEVICE_PTR_VA   0x002BAD98u   /* D3D: g_pDevice */
#define DW4_D3D_DEV_SEMAPHORE   0x30u         /* device+0x30: ponteiro do semaforo */

static inline uint32_t guest_read32(uint32_t va)
{
    return *(volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va);
}

/* ── "Me acorde" do fence (metodo de software 5) ───────────────────────────
 *
 * Quando julga a GPU muito atrasada (>= 0x2000 bytes), D3D_BlockOnTime grava
 * NO_OPERATION(5) no pushbuffer na posicao do fence e dorme em
 * KeWaitForSingleObject(device + 0x1DCC) (guest 0x002B1648). No console a GPU
 * interrompe ao executar esse metodo e o handler do D3D sinaliza o evento.
 * O runtime nao levanta essa interrupcao, entao o executor chama este hook e
 * sinalizamos o evento direto. */
#define DW4_D3D_DEV_WAKE_EVENT  0x1DCCu

extern int xbox_KeSetGuestEvent(uint32_t guest_va);

static void wake_fence_waiter(void)
{
    uint32_t dev = guest_read32(DW4_D3D_DEVICE_PTR_VA);

    if (dev >= 0x10000 && (size_t)dev + 0x2000 < xbox_GetMappedSize())
        xbox_KeSetGuestEvent(dev + DW4_D3D_DEV_WAKE_EVENT);
}

static void on_software_method(uint32_t param)
{
    if (param == 5)
        wake_fence_waiter();
}

static void *fence_watch_thread(void *arg)
{
    uint32_t current = 0, idle_put = 0;
    int idle_polls = 0;
    struct timespec ts = { 0, 1000000 };   /* 1 ms */

    (void)arg;
    for (;;) {
        uint32_t dev = guest_read32(DW4_D3D_DEVICE_PTR_VA);

        if (dev >= 0x10000 && (size_t)dev + 0x40 < xbox_GetMappedSize()) {
            uint32_t sem = guest_read32(dev + DW4_D3D_DEV_SEMAPHORE);

            if (sem && sem != current) {
                nv2a_pb_set_semaphore_target(sem);
                fprintf(stderr, "[D3D] semaforo da GPU em 0x%08X (device 0x%08X)\n",
                        sem, dev);
                current = sem;
                /* depois de encontrado, basta conferir de vez em quando */
                ts.tv_nsec = 50000000;
            }
        }
        /* Rede de seguranca: o D3D pode gravar o "me acorde" numa posicao
         * que o executor ja passou (o DMA_GET que ele le anda atrasado), e
         * ai o metodo nunca e executado. Se a GPU esta parada com tudo
         * consumido (PUT == GET, sem mudar entre duas leituras) e o evento
         * continua nao sinalizado, sinalizar e dizer a verdade. */
        if (current) {
            volatile uint32_t *put = xbox_Nv2aRegPtr(0x800040);
            volatile uint32_t *get = xbox_Nv2aRegPtr(0x800044);

            if (put && get && *put == *get && *put == idle_put) {
                if (++idle_polls >= 2 && dev >= 0x10000
                        && guest_read32(dev + DW4_D3D_DEV_WAKE_EVENT + 4) == 0) {
                    wake_fence_waiter();
                    idle_polls = 0;
                }
            } else {
                idle_polls = 0;
                idle_put = put ? *put : 0;
            }
        }
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void dw4_d3d_fence_init(void)
{
    pthread_t t;

    nv2a_pb_set_software_method_hook(on_software_method);
    if (pthread_create(&t, NULL, fence_watch_thread, NULL) == 0)
        pthread_detach(t);
}
