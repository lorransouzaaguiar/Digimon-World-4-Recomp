/**
 * Back end Vulkan para o executor de pushbuffer NV2A (nv2a_backend.h).
 *
 * Irmao do back end OpenGL (nv2a_gl_backend.c): mesma interface, mesmo
 * desenho, mesmo shader de combiners. O jogador escolhe entre os dois no
 * launcher (ou DW4_RENDERER=vulkan|opengl); o host (gl_host.c) carrega este
 * arquivo como libdw4_vk.so.
 *
 * Ideias tiradas dos ports de referencia que ja usam Vulkan nesta interface
 * (Amped 2 / SSX 3, docs/REFERENCIAS.md) -- implementacao propria, nada
 * copiado (sao GPL-3): Vulkan-Headers fixado em third_party/, shaders GLSL
 * compilados com `glslc -mfmt=c`, estado dos combiners num UBO por lote
 * reaproveitado enquanto nao muda, hash de textura conferido uma vez por
 * frame, paleta na chave da textura.
 *
 * O que e diferente deles, de proposito:
 *   - dois jeitos de apresentar (ver "Apresentacao" mais abaixo):
 *       direto   com janela: swapchain proprio na janela do toolkit; o frame
 *                nao sai da GPU e o envio nao espera (dois conjuntos de
 *                recursos de frame, "quadros em voo"). Pixels so sao lidos de
 *                volta quando alguem pede (captura, roteiro);
 *       leitura  sem janela (soak, goldens) ou se o swapchain nao sobe: como
 *                no OpenGL, o frame e lido de volta para a memoria do guest e
 *                a janela SDL do toolkit apresenta;
 *   - Vulkan 1.0 puro, sem extensoes: o estado que o 1.0 nao tem como dinamico
 *     (blend, profundidade, stencil, cull, mascara de cor) vira um cache de
 *     pipelines. Roda em qualquer driver Vulkan, nao so nos com 1.3 + EDS3.
 *
 *   superficies   uma imagem de cor por superficie do guest (chave: color_va),
 *                 sempre em layout GENERAL (e alvo e textura ao longo do frame);
 *                 depth/stencil por zeta_va
 *   texturas      nv2a_backend_decode_texture -> BGRA8, cache por (endereco,
 *                 formato, tamanho, paleta) + hash conferido uma vez por frame
 *   lotes         draws consecutivos com o mesmo estado saem num vkCmdDraw
 *   frame         dois command buffers: "upload" (texturas) e "draw", enviados
 *                 nessa ordem no flip. No modo direto ha dois conjuntos
 *                 (buffers de comando, aneis, descritores) e o flip nao espera
 *                 a GPU; no modo leitura, um conjunto e espera no flip
 *
 * Convencoes (diferem do OpenGL): y do NDC para baixo -- a linha 0 da imagem e
 * a linha 0 da superficie, sem espelhamento, entao o sentido de giro da NV2A
 * vale direto (sem a troca CW/CCW do back end OpenGL); z do NDC em [0,1].
 *
 *   DW4_VK_DEVICE=<n>   escolhe a placa pelo indice (padrao: dedicada primeiro)
 *   DW4_VK_ALLOW_SW=1   aceita Vulkan por software (lavapipe)
 *   DW4_VK_CULL=0|inv   desliga / inverte o culling (depuracao)
 *   DW4_VK_STATS=1      contadores a cada 5 s
 *   DW4_VK_PRESENT=     readback: sempre o modo leitura; async: quadros em voo
 *                       mesmo sem janela (teste); padrao: direto se houver janela
 *   DW4_VSYNC=          auto (padrao: mailbox, senao fifo) | on (fifo) |
 *                       off (immediate, senao mailbox)
 *   F5..F8, F10, F11    os mesmos alternadores do OpenGL (a captura F9 e a
 *                       sonda de pixel existem so no OpenGL)
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vulkan/vulkan.h>

#include "nv2a_backend.h"
#include "xbox_memory_layout.h"
#include "dw4_hooks.h"
#include "gl_module.h"
#include "render_opts.h"

#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)

static const uint32_t VS_SPV[] =
#include "nv2a_vk.vert.inc"
;
static const uint32_t FS_SPV[] =
#include "nv2a_vk.frag.inc"
;

/* Espelha o bloco Batch de shaders/nv2a_ubo.glsl (std140). */
typedef struct {
    float    surf[4];
    float    texsize[4];
    int32_t  alpha[4];
    float    tscale[4][4];
    float    fogc[4];
    uint32_t cw[8][4];
    float    f0[8][4];
    float    f1[8][4];
    float    fc0[4];
    float    fc1[4];
    uint32_t fin[4];
    uint32_t ctl[4];
} Ubo;

#define VTX_CAP     524288u             /* vertices por envio */
#define PENDING_MAX 196608u
#define UBO_CAP     (8u * 1024u * 1024u)
#define MAX_SETS    8192u
#define SET_CACHE   1024u
#define COLOR_FMT   VK_FORMAT_B8G8R8A8_UNORM   /* 0xAARRGGBB na memoria */

/* ── Dispositivo ───────────────────────────────────────────────────────────── */

static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkFormat s_depth_fmt;
static int s_depth_clamp;
static VkCommandPool s_pool;
/* Do conjunto de frame corrente (ver "Conjuntos de frame"). */
static VkCommandBuffer s_cmd_up, s_cmd;     /* texturas; desenho */
static VkFence s_fence;
static VkRenderPass s_rp[2];                /* [0] so cor, [1] cor + depth */
static VkDescriptorSetLayout s_dsl;
static VkPipelineLayout s_layout;
static VkDescriptorPool s_dpool;
static VkShaderModule s_vs, s_fs;
static VkSampler s_samp[3][3][2];
static int s_ok;                            /* init completo */
static int s_lost;                          /* erro fatal do dispositivo: para de desenhar */

static int s_cull_mode = 1;                 /* 0 off, 1 normal, 2 invertido */
static int s_stats, s_dbg, s_skip_depth_only, s_no_poly_offset;
static uint32_t s_frame;
static unsigned long s_n_decode, s_n_draw, s_n_tris, s_n_flush, s_n_submit;
/* Onde vai o tempo da thread do executor (DW4_VK_STATS): total dentro do back
 * end, espera pela GPU (envio + fence) e copia do frame para a janela. */
static double s_t_backend, s_t_gpu, s_t_copy;
static unsigned long s_n_readback;          /* frames lidos de volta para a CPU */
static unsigned long s_n_odd_present;       /* frames exibidos de superficie nao-tela */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define VKCHECK(call) do {                                              \
        VkResult _r = (call);                                           \
        if (_r != VK_SUCCESS) {                                         \
            fprintf(stderr, "[VK] %s falhou (%d)\n", #call, (int)_r);   \
            s_lost = 1;                                                 \
        }                                                               \
    } while (0)

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} Buf;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
} Img;

static Buf s_vb, s_vbe, s_dummy, s_ubo, s_stage, s_rb;
static uint32_t s_vtx_used, s_ubo_used, s_ubo_step, s_sets_used;
static VkDeviceSize s_stage_used;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i))
                && (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int s_buf_cached;                /* proximo buffer: memoria boa de LER na CPU */

static int buf_create(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements mr;

    memset(b, 0, sizeof *b);
    if (vkCreateBuffer(s_dev, &bi, NULL, &b->buf) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(s_dev, b->buf, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = UINT32_MAX;
    if (s_buf_cached)                   /* leitura de volta: sem write-combining */
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
            | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    s_buf_cached = 0;
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(s_dev, &ai, NULL, &b->mem) != VK_SUCCESS
            || vkBindBufferMemory(s_dev, b->buf, b->mem, 0) != VK_SUCCESS
            || vkMapMemory(s_dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
        fprintf(stderr, "[VK] sem memoria para um buffer de %llu bytes\n",
                (unsigned long long)size);
        return 0;
    }
    b->size = size;
    return 1;
}

static void buf_destroy(Buf *b)
{
    if (b->buf) vkDestroyBuffer(s_dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(s_dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

static int img_create(Img *im, uint32_t w, uint32_t h, VkFormat fmt,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { aspect, 0, 1, 0, 1 } };
    VkMemoryRequirements mr;

    memset(im, 0, sizeof *im);
    if (vkCreateImage(s_dev, &ci, NULL, &im->img) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(s_dev, im->img, &mr);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX)
        ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, 0);
    vi.image = im->img;
    if (vkAllocateMemory(s_dev, &ai, NULL, &im->mem) != VK_SUCCESS
            || vkBindImageMemory(s_dev, im->img, im->mem, 0) != VK_SUCCESS
            || vkCreateImageView(s_dev, &vi, NULL, &im->view) != VK_SUCCESS) {
        fprintf(stderr, "[VK] sem memoria para uma imagem %ux%u\n", w, h);
        s_lost = 1;
        return 0;
    }
    return 1;
}

static void img_destroy(Img *im)
{
    if (im->view) vkDestroyImageView(s_dev, im->view, NULL);
    if (im->img) vkDestroyImage(s_dev, im->img, NULL);
    if (im->mem) vkFreeMemory(s_dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
                    VkImageLayout from, VkImageLayout to,
                    VkAccessFlags sa, VkAccessFlags da,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = sa, .dstAccessMask = da, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = { aspect, 0, 1, 0, 1 } };

    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* ── Conjuntos de frame ────────────────────────────────────────────────────
 * Tudo o que um frame grava e a GPU ainda le depois do envio: os dois command
 * buffers, os aneis de vertices / uniformes / staging e os descritores. Com
 * um conjunto so, cada flip espera a GPU terminar (modo leitura). Com dois, o
 * flip envia e ja comeca o frame seguinte no outro conjunto, esperando apenas
 * se a GPU estiver um frame inteiro atrasada: CPU e GPU trabalham ao mesmo
 * tempo. Os globais s_cmd, s_vb, s_dpool... sao sempre os do conjunto
 * corrente (set_load / set_save).
 *
 * Destruir uma imagem ou um buffer continua exigindo a GPU parada:
 * frame_submit() (sem "async") espera TODOS os conjuntos. */
#define FRAME_SETS 2

typedef struct {
    VkCommandBuffer up, draw;
    VkFence fence;
    int pending;                        /* enviado; o fence ainda nao foi conferido */
    VkDescriptorPool dpool;
    Buf vb, vbe, ubo, stage;
    VkSemaphore acquired, rendered;     /* imagem do swapchain: obtida / desenhada */
} FrameSet;

static FrameSet s_sets[FRAME_SETS];
static int s_nsets, s_set;
static int s_async;                     /* 1: o flip nao espera a GPU */

static void set_save(void)
{
    FrameSet *f = &s_sets[s_set];

    f->up = s_cmd_up; f->draw = s_cmd; f->fence = s_fence; f->dpool = s_dpool;
    f->vb = s_vb; f->vbe = s_vbe; f->ubo = s_ubo; f->stage = s_stage;
}

static void set_load(int i)
{
    const FrameSet *f = &s_sets[i];

    s_set = i;
    s_cmd_up = f->up; s_cmd = f->draw; s_fence = f->fence; s_dpool = f->dpool;
    s_vb = f->vb; s_vbe = f->vbe; s_ubo = f->ubo; s_stage = f->stage;
}

static void set_wait(FrameSet *f)
{
    if (!f->pending)
        return;
    VKCHECK(vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
    VKCHECK(vkResetFences(s_dev, 1, &f->fence));
    f->pending = 0;
}

/* ── Apresentacao direta (swapchain) ───────────────────────────────────────
 * A janela e do toolkit (fb_present_sdl.c, RECOMP_WINDOW_VULKAN=1); aqui so
 * a superficie e o swapchain. WSI e extensao por definicao (VK_KHR_surface,
 * a da plataforma e VK_KHR_swapchain): sao as unicas que este back end pede,
 * e so para apresentar -- sem elas ele cai no modo leitura, que e Vulkan 1.0
 * puro. */
extern int  xbox_FramebufferWindowVulkan(void);
extern int  xbox_FramebufferWindowVkExtensions(const char **names, unsigned *count);
extern int  xbox_FramebufferWindowVkSurface(void *instance, void *surface_out);
extern void xbox_FramebufferWindowVkRelease(void);
extern void xbox_FramebufferWindowVkDrawableSize(int *w, int *h);
extern void xbox_FramebufferWindowFrameShape(uint32_t w, uint32_t h);

#define SWAP_MAX 8
#define SWAP_NONE 0xFFFFFFFFu

static struct {
    VkSurfaceKHR surf;
    VkSwapchainKHR sc;
    VkImage img[SWAP_MAX];
    uint32_t n, w, h;
    VkFormat fmt;
    VkPresentModeKHR mode;
    int have_ext;                       /* o dispositivo tem VK_KHR_swapchain */
    int ok;                             /* da para apresentar */
    int stale;                          /* refazer antes do proximo frame */
    int shot;                           /* DW4_VK_SHOT_SWAP: capturas leem o swapchain */
    uint32_t acquired;                  /* imagem obtida neste frame, ou SWAP_NONE */
    unsigned fails;
} s_swap = { .acquired = SWAP_NONE };

static void sets_wait_all(void);

static void swap_destroy(int with_surface)
{
    if (s_dev && s_swap.sc) {
        vkDeviceWaitIdle(s_dev);
        vkDestroySwapchainKHR(s_dev, s_swap.sc, NULL);
    }
    s_swap.sc = VK_NULL_HANDLE;
    s_swap.ok = 0;
    s_swap.n = 0;
    s_swap.acquired = SWAP_NONE;
    if (with_surface && s_swap.surf) {
        vkDestroySurfaceKHR(s_inst, s_swap.surf, NULL);
        s_swap.surf = VK_NULL_HANDLE;
        xbox_FramebufferWindowVkRelease();
    }
}

static const char *present_mode_name(VkPresentModeKHR m)
{
    switch (m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "immediate (sem vsync)";
    case VK_PRESENT_MODE_MAILBOX_KHR:      return "mailbox (vsync sem fila)";
    case VK_PRESENT_MODE_FIFO_KHR:         return "fifo (vsync)";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo relaxed";
    default:                               return "?";
    }
}

/* Cria (ou refaz, apos mudanca de tamanho) o swapchain. A GPU precisa estar
 * parada: quem chama garante. 0 = nao deu; o chamador decide se desiste. */
static int swap_create(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[64];
    VkPresentModeKHR modes[16];
    uint32_t nf = 64, nm = 16, want_n;
    VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = s_swap.sc;
    const char *vs = getenv("DW4_VSYNC"), *legacy = getenv("RECOMP_PRESENT_VSYNC");
    int pref = 0, dw = 0, dh = 0, have[4] = { 0, 0, 0, 0 };   /* 0 auto, 1 on, 2 off */
    VkResult r;

    s_swap.ok = 0;
    s_swap.stale = 0;
    if (!s_swap.surf || !s_swap.have_ext)
        return 0;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_swap.surf, &caps) != VK_SUCCESS)
        return 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_swap.surf, &nf, fmts) < 0 || !nf)
        return 0;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_swap.surf, &nm, modes) < 0)
        nm = 0;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "[VK] a janela nao aceita copia para o swapchain\n");
        return 0;
    }

    /* Formato sem conversao sRGB: as cores ja saem prontas do shader. */
    ci.imageFormat = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < nf && ci.imageFormat == VK_FORMAT_UNDEFINED; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            ci.imageFormat = fmts[i].format; ci.imageColorSpace = fmts[i].colorSpace;
        }
    for (uint32_t i = 0; i < nf && ci.imageFormat == VK_FORMAT_UNDEFINED; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM
                || fmts[i].format == VK_FORMAT_A8B8G8R8_UNORM_PACK32) {
            ci.imageFormat = fmts[i].format; ci.imageColorSpace = fmts[i].colorSpace;
        }
    if (ci.imageFormat == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[VK] a janela so oferece formatos sRGB/HDR (primeiro: %d)\n",
                (int)fmts[0].format);
        return 0;
    }
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(s_phys, ci.imageFormat, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            return 0;
    }

    /* Vsync. O jogo ja e cadenciado pelo vblank emulado (60 Hz do relogio),
     * entao o modo aqui so decide o que acontece quando um quadro chega fora
     * de fase com o monitor: mailbox troca sem rasgar e sem segurar o
     * executor; fifo segura ate o proximo vblank real; immediate rasga. */
    if (vs && *vs)
        pref = !strcmp(vs, "on") || !strcmp(vs, "fifo") ? 1
             : (!strcmp(vs, "off") || !strcmp(vs, "0") || !strcmp(vs, "immediate")) ? 2 : 0;
    else if (legacy && legacy[0] == '0')
        pref = 2;
    for (uint32_t i = 0; i < nm; i++)
        if ((uint32_t)modes[i] < 4)
            have[modes[i]] = 1;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;          /* sempre existe */
    if (pref == 2 && have[VK_PRESENT_MODE_IMMEDIATE_KHR])
        ci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    else if (pref != 1 && have[VK_PRESENT_MODE_MAILBOX_KHR])
        ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;

    xbox_FramebufferWindowVkDrawableSize(&dw, &dh);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        ci.imageExtent = caps.currentExtent;
    } else {
        ci.imageExtent.width = dw > 0 ? (uint32_t)dw : 640;
        ci.imageExtent.height = dh > 0 ? (uint32_t)dh : 480;
    }
    if (ci.imageExtent.width < caps.minImageExtent.width)
        ci.imageExtent.width = caps.minImageExtent.width;
    if (ci.imageExtent.height < caps.minImageExtent.height)
        ci.imageExtent.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ci.imageExtent.width > caps.maxImageExtent.width)
        ci.imageExtent.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ci.imageExtent.height > caps.maxImageExtent.height)
        ci.imageExtent.height = caps.maxImageExtent.height;
    if (!ci.imageExtent.width || !ci.imageExtent.height)
        return 0;                       /* minimizada: tenta de novo depois */

    want_n = caps.minImageCount + 1;
    if (caps.maxImageCount && want_n > caps.maxImageCount)
        want_n = caps.maxImageCount;
    ci.surface = s_swap.surf;
    ci.minImageCount = want_n;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    /* Teste: as capturas passam a ser da imagem do swapchain (o que a janela
     * mostra, com escala e faixas), nao da superficie do jogo. */
    s_swap.shot = getenv("DW4_VK_SHOT_SWAP") != NULL
               && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (s_swap.shot)
        ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                      ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                      : VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;

    r = vkCreateSwapchainKHR(s_dev, &ci, NULL, &s_swap.sc);
    if (old)
        vkDestroySwapchainKHR(s_dev, old, NULL);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateSwapchainKHR falhou (%d)\n", (int)r);
        s_swap.sc = VK_NULL_HANDLE;
        return 0;
    }
    s_swap.n = SWAP_MAX;
    r = vkGetSwapchainImagesKHR(s_dev, s_swap.sc, &s_swap.n, s_swap.img);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !s_swap.n) {
        /* VK_INCOMPLETE: mais imagens que SWAP_MAX; sem todas nao da para usar */
        fprintf(stderr, "[VK] swapchain com imagens demais ou sem imagens (%d)\n", (int)r);
        vkDestroySwapchainKHR(s_dev, s_swap.sc, NULL);
        s_swap.sc = VK_NULL_HANDLE;
        return 0;
    }
    if (r == VK_INCOMPLETE) {
        vkDestroySwapchainKHR(s_dev, s_swap.sc, NULL);
        s_swap.sc = VK_NULL_HANDLE;
        return 0;
    }
    if (!s_swap.fmt || s_swap.w != ci.imageExtent.width || s_swap.h != ci.imageExtent.height
            || s_swap.mode != ci.presentMode)
        fprintf(stderr, "[VK] swapchain %ux%u, %u imagens, %s\n", ci.imageExtent.width,
                ci.imageExtent.height, s_swap.n, present_mode_name(ci.presentMode));
    s_swap.w = ci.imageExtent.width;
    s_swap.h = ci.imageExtent.height;
    s_swap.fmt = ci.imageFormat;
    s_swap.mode = ci.presentMode;
    s_swap.acquired = SWAP_NONE;
    s_swap.ok = 1;
    return 1;
}

/* ── Frame: gravacao e envio ───────────────────────────────────────────────── */

typedef struct Surface Surface;
typedef struct Zeta Zeta;

static int s_recording;
static VkFramebuffer s_pass_fb;             /* render pass aberto (0 = nenhum) */
static VkPipeline s_bound_pipe;
static int s_bound_ext = -1;                /* binding 1: 1 = ext, 0 = dummy */
static Ubo s_last_ubo;
static uint32_t s_last_ubo_off;
static int s_last_ubo_valid;

typedef struct {
    VkImageView view[4];
    VkSampler samp[4];
    VkDescriptorSet set;
} SetEntry;
static SetEntry s_setc[SET_CACHE];

static void frame_begin(void)
{
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

    if (s_recording)
        return;
    VKCHECK(vkResetDescriptorPool(s_dev, s_dpool, 0));
    memset(s_setc, 0, sizeof s_setc);
    VKCHECK(vkBeginCommandBuffer(s_cmd_up, &bi));
    VKCHECK(vkBeginCommandBuffer(s_cmd, &bi));
    s_vtx_used = s_ubo_used = s_sets_used = 0;
    s_stage_used = 0;
    s_pass_fb = VK_NULL_HANDLE;
    s_bound_pipe = VK_NULL_HANDLE;
    s_bound_ext = -1;
    s_last_ubo_valid = 0;
    s_recording = 1;
}

static void end_pass(void)
{
    if (s_pass_fb) {
        vkCmdEndRenderPass(s_cmd);
        s_pass_fb = VK_NULL_HANDLE;
    }
}

static void sets_wait_all(void)
{
    for (int i = 0; i < s_nsets; i++)
        set_wait(&s_sets[i]);
}

/* Envia o que foi gravado.
 *   async = 0: espera terminar. Depois disto nada na GPU usa mais nenhum
 *              recurso (nem os frames anteriores ainda em voo): e o ponto
 *              seguro para destruir imagens.
 *   async = 1: nao espera; passa a gravar no outro conjunto de frame (e so
 *              espera se esse ainda estiver em uso, isto e, a GPU um frame
 *              inteiro atrasada).
 * Se ha uma imagem do swapchain obtida neste frame, ela e apresentada. */
static void frame_submit_ex(int async)
{
    VkCommandBuffer cbs[2] = { s_cmd_up, s_cmd };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 2, .pCommandBuffers = cbs };
    FrameSet *f = &s_sets[s_set];
    double t0;

    if (!s_recording) {
        if (!async)
            sets_wait_all();
        return;
    }
    end_pass();
    VKCHECK(vkEndCommandBuffer(s_cmd_up));
    VKCHECK(vkEndCommandBuffer(s_cmd));
    s_recording = 0;
    if (s_lost)
        return;
    if (s_swap.acquired != SWAP_NONE) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f->acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f->rendered;
    }
    t0 = now_s();
    VKCHECK(vkQueueSubmit(s_queue, 1, &si, s_fence));
    if (s_lost)
        return;
    f->pending = 1;
    s_n_submit++;
    if (s_swap.acquired != SWAP_NONE) {
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &f->rendered,
            .swapchainCount = 1, .pSwapchains = &s_swap.sc,
            .pImageIndices = &s_swap.acquired };
        VkResult r = vkQueuePresentKHR(s_queue, &pi);

        s_swap.acquired = SWAP_NONE;
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            s_swap.stale = 1;
        } else if (r != VK_SUCCESS) {
            fprintf(stderr, "[VK] vkQueuePresentKHR falhou (%d)\n", (int)r);
            s_swap.stale = 1;
            s_swap.fails++;
        }
    }
    if (async && s_nsets > 1) {
        set_save();
        set_load((s_set + 1) % s_nsets);
        set_wait(&s_sets[s_set]);
    } else {
        sets_wait_all();
    }
    s_t_gpu += now_s() - t0;
}

static void frame_submit(void)
{
    frame_submit_ex(0);
}

/* ── Superficies ───────────────────────────────────────────────────────────── */

struct Surface {
    uint32_t va, w, h, pitch, bpp, clip_h;  /* do guest */
    uint32_t pw, ph;                    /* imagem da GPU (render_opts.h) */
    int wide;                           /* superficie de tela alargada */
    Img color;
    int dirty;                          /* desenhada desde a ultima barreira de leitura */
};

struct Zeta {
    uint32_t va, w, h;                  /* w, h: pixels da imagem */
    Img img;
};

typedef struct {
    Surface *surf;
    Zeta *zeta;
    VkFramebuffer fb;
} FbEntry;

#define MAX_SURF 64
#define MAX_FB   256
static Surface s_surf[MAX_SURF];
static int s_nsurf;
static Zeta s_zeta[MAX_SURF];
static int s_nzeta;
static FbEntry s_fb[MAX_FB];
static int s_nfb;
static Surface *s_shown;                /* a maior superficie desenhada no frame */
static int s_shown_stale = 1;

static void flush_pending(void);

static uint32_t pow2_ceil(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* So com a GPU parada (depois de frame_submit). */
static void fb_drop(const Surface *surf, const Zeta *zeta)
{
    for (int i = 0; i < s_nfb; ) {
        if ((surf && s_fb[i].surf == surf) || (zeta && s_fb[i].zeta == zeta)) {
            vkDestroyFramebuffer(s_dev, s_fb[i].fb, NULL);
            s_fb[i] = s_fb[--s_nfb];
        } else {
            i++;
        }
    }
}

static void surface_free(Surface *e)
{
    flush_pending();
    frame_submit();
    fb_drop(e, NULL);
    img_destroy(&e->color);
    frame_begin();
}

static Surface *surface_get(const Nv2aSurface *s)
{
    uint32_t bpp = s->bytes_per_pixel ? s->bytes_per_pixel : 4;
    uint32_t w = s->pitch / bpp, h = s->height;
    Surface *e = NULL;

    if (w < s->width)
        w = s->width;
    /* A interface da o retangulo de clip, nao o tamanho da superficie. Um
     * alvo de render-to-texture e potencia de dois com o clip recuado
     * (512x512 com clip 498x498+7+7 no lobby). Mesma regra do OpenGL. */
    if ((w & (w - 1)) == 0 && h <= w && h > w / 2)
        h = w;
    else if ((w & (w - 1)) == 0 && h < w)
        h = pow2_ceil(h);
    if (!w) w = 1;
    if (!h) h = 1;

    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == s->color_va) { e = &s_surf[i]; break; }
    if (e && e->color.img && (e->w != w || e->h < h))    /* mudou de tamanho */
        surface_free(e);
    if (!e) {
        if (s_nsurf == MAX_SURF) {            /* raro: recicla a mais antiga */
            e = &s_surf[0];
            surface_free(e);
            if (s_shown == e) s_shown = NULL;
        } else {
            e = &s_surf[s_nsurf++];
        }
        memset(e, 0, sizeof *e);
    }
    if (!e->color.img) {
        static const VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
        static const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

        e->va = s->color_va; e->w = w; e->h = h; e->dirty = 0;
        e->wide = ro_is_screen(w, h);
        ro_extent(w, h, e->wide, &e->pw, &e->ph);
        if (ro_enhanced())
            fprintf(stderr, "[VK] superficie %08X %ux%u (clip %ux%u) -> imagem %ux%u%s\n",
                    e->va, w, h, s->width, s->height, e->pw, e->ph, e->wide ? " (tela)" : "");
        if (!img_create(&e->color, e->pw, e->ph, COLOR_FMT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT))
            return NULL;
        end_pass();
        barrier(s_cmd, e->color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdClearColorImage(s_cmd, e->color.img, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &range);
    }
    e->pitch = s->pitch; e->bpp = bpp; e->clip_h = s->height;
    return e;
}

static Zeta *zeta_get(uint32_t va, const Surface *e)
{
    static const VkClearDepthStencilValue far_ = { 1.0f, 0 };
    static const VkImageSubresourceRange range = {
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1 };
    Zeta *z = NULL;

    for (int i = 0; i < s_nzeta; i++)
        if (s_zeta[i].va == va && s_zeta[i].w == e->pw && s_zeta[i].h == e->ph)
            return &s_zeta[i];
    if (s_nzeta == MAX_SURF) {
        z = &s_zeta[0];
        frame_submit();
        fb_drop(NULL, z);
        img_destroy(&z->img);
        frame_begin();
    } else {
        z = &s_zeta[s_nzeta++];
    }
    z->va = va; z->w = e->pw; z->h = e->ph;
    if (!img_create(&z->img, e->pw, e->ph, s_depth_fmt,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    range.aspectMask)) {
        z->va = 0; z->w = 0;
        return NULL;
    }
    end_pass();
    barrier(s_cmd, z->img.img, range.aspectMask, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearDepthStencilImage(s_cmd, z->img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                &far_, 1, &range);
    barrier(s_cmd, z->img.img, range.aspectMask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    return z;
}

/* Abre (ou mantem) o render pass sobre a superficie + zeta. */
static int begin_pass(Surface *e, Zeta *z)
{
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkRenderPassBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = s_rp[z != NULL], .renderArea = { { 0, 0 }, { e->pw, e->ph } } };
    VkViewport vp = { 0, 0, (float)e->pw, (float)e->ph, 0.0f, 1.0f };

    for (int i = 0; i < s_nfb; i++)
        if (s_fb[i].surf == e && s_fb[i].zeta == z) { fb = s_fb[i].fb; break; }
    if (!fb) {
        VkImageView att[2] = { e->color.view, z ? z->img.view : VK_NULL_HANDLE };
        VkFramebufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = s_rp[z != NULL], .attachmentCount = z ? 2 : 1,
            .pAttachments = att, .width = e->pw, .height = e->ph, .layers = 1 };

        if (s_nfb == MAX_FB) {                /* nunca visto; recomeca o cache */
            frame_submit();
            for (int i = 0; i < s_nfb; i++)
                vkDestroyFramebuffer(s_dev, s_fb[i].fb, NULL);
            s_nfb = 0;
            frame_begin();
        }
        if (vkCreateFramebuffer(s_dev, &ci, NULL, &fb) != VK_SUCCESS) {
            s_lost = 1;
            return 0;
        }
        s_fb[s_nfb++] = (FbEntry){ e, z, fb };
    }
    if (s_pass_fb == fb)
        return 1;
    end_pass();
    bi.framebuffer = fb;
    vkCmdBeginRenderPass(s_cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
    vkCmdSetScissor(s_cmd, 0, 1, &bi.renderArea);
    s_pass_fb = fb;
    return 1;
}

static void note_shown(Surface *e)
{
    /* Pela area da superficie, nao pelo recorte do ultimo draw: com o recorte
     * (um modelo num sub-viewport de 150x105 deixa clip_h = 105) um alvo de
     * render-to-texture 512x512 desenhado depois "ganhava" da tela e era
     * exibido esticado por um frame -- a tela piscava distorcida. */
    if (!s_shown_stale && s_shown && e->w * e->h < s_shown->w * s_shown->h)
        return;
    s_shown_stale = 0;
    s_shown = e;
}

/* ── Texturas ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t offset, w, h, color, palette;
    Img img;
    uint64_t hash;
    uint32_t checked_frame;
    int used;
} TexEntry;

#define TEX_SLOTS 2048
static TexEntry s_tex[TEX_SLOTS];
static int s_tex_count;
static unsigned s_tex_gen;              /* muda quando o cache e esvaziado */
static uint32_t *s_decode;
static size_t s_decode_cap;
static Img s_white;

static uint32_t tex_va(uint32_t offset)
{
    if (offset >= CONTIG_BASE)
        return offset;
    if (offset < xbox_ContiguousAllocatedBytes())
        return CONTIG_BASE + offset;
    return offset;
}

static const uint8_t *guest_bytes(uint32_t va, size_t n)
{
    if (va >= CONTIG_BASE) {
        if ((uint64_t)va + n > (uint64_t)CONTIG_BASE + CONTIG_SIZE)
            return NULL;
    } else if ((uint64_t)va + n > xbox_GetMappedSize() || va < 0x10000) {
        return NULL;
    }
    return (const uint8_t *)xbox_GetMemoryOffset() + va;
}

/* Bytes do nivel 0, pelo formato NV097 (por baixo quando o formato nao e
 * conhecido: ler alem da textura faria ela "mudar" todo frame). */
static size_t tex_bytes(const Nv2aTexture *t)
{
    size_t px = (size_t)t->width * t->height;

    if (t->pitch)
        return (size_t)t->pitch * t->height;
    switch (t->color) {
    case 0x0C: return px / 2;                         /* DXT1 */
    case 0x0E: case 0x0F: return px;                  /* DXT3 / DXT5 */
    case 0x06: case 0x07: case 0x12: case 0x1E: case 0x3A: case 0x3B:
        return px * 4;                                /* 8888 */
    case 0x0B: case 0x00: case 0x01: case 0x19: return px;   /* 8 bits */
    default:   return px * 2;                         /* 16 bits */
    }
}

static uint64_t hash_bytes(const uint8_t *p, size_t n)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t words = n / 8, step = words > 32768 ? words / 32768 : 1;

    for (size_t i = 0; i < words; i += step) {
        uint64_t v;
        memcpy(&v, p + i * 8, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    return h;
}

/* Copia w*h pixels BGRA para a imagem, pelo command buffer de upload (que roda
 * antes de todos os draws deste envio). fresh: a imagem acabou de ser criada. */
static void tex_upload(Img *im, const uint32_t *px, uint32_t w, uint32_t h, int fresh)
{
    VkDeviceSize need = (VkDeviceSize)w * h * 4;
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };

    if (need > s_stage.size || s_stage_used + need > s_stage.size) {
        /* Sem espaco: envia o que ha (o lote pendente continua na CPU e as
         * imagens que ele usa continuam vivas) e recomeca o anel. */
        frame_submit();
        if (need > s_stage.size) {
            buf_destroy(&s_stage);
            if (!buf_create(&s_stage, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
                s_lost = 1;
                return;
            }
        }
        frame_begin();
    }
    memcpy((uint8_t *)s_stage.map + s_stage_used, px, (size_t)need);
    region.bufferOffset = s_stage_used;
    s_stage_used += (need + 15) & ~(VkDeviceSize)15;

    barrier(s_cmd_up, im->img, VK_IMAGE_ASPECT_COLOR_BIT,
            fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            fresh ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            fresh ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdCopyBufferToImage(s_cmd_up, s_stage.buf, im->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(s_cmd_up, im->img, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/* Devolve a view a amostrar; *from_surf recebe a superficie quando a textura
 * e um alvo de render (render-to-texture), senao NULL. */
static VkImageView texture_get(const Nv2aTexture *t, const Surface *target,
                               Surface **from_surf)
{
    uint32_t va = tex_va(t->offset);
    uint32_t pal_key = 0, idx;
    TexEntry *e = NULL;
    const uint8_t *bytes;
    size_t nbytes;
    uint64_t h = 0;
    int fresh = 0;

    *from_surf = NULL;
    /* Paleta: identificada pelo CONTEUDO, conferido a cada uso (o jogo troca
     * so a paleta entre draws do mesmo frame). Ver nv2a_gl_backend.c. */
    /* So para formato paletizado (P8, 0x0B): o campo palette fica com o valor
     * da ultima paleta ligada, e uma textura DXT ganhava uma copia nova no
     * cache toda vez que aquela paleta (que ela nem usa) mudava. */
    if (t->palette && t->color == 0x0B) {
        const uint8_t *pal = guest_bytes(t->palette, 1024);
        uint64_t ph = pal ? hash_bytes(pal, 1024) : t->palette;
        pal_key = (uint32_t)(ph ^ (ph >> 32)) | 1u;
    }
    idx = (va * 2654435761u ^ t->width * 31u ^ t->height * 131u
           ^ t->color * 0x9E37u ^ pal_key * 0x85EBCA6Bu) % TEX_SLOTS;

    for (int i = 0; i < s_nsurf; i++) {
        if (s_surf[i].va == va && s_surf[i].color.img) {
            if (&s_surf[i] == target)
                return s_white.view;    /* amostrar o proprio alvo: indefinido */
            *from_surf = &s_surf[i];
            return s_surf[i].color.view;
        }
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return s_white.view;

    for (uint32_t probe = 0; probe < TEX_SLOTS; probe++) {
        TexEntry *c = &s_tex[(idx + probe) % TEX_SLOTS];
        if (!c->used) { e = c; break; }
        if (c->offset == va && c->w == t->width && c->h == t->height
                && c->color == t->color && c->palette == pal_key) {
            e = c;
            break;
        }
    }
    if (!e || (!e->used && s_tex_count > TEX_SLOTS * 3 / 4)) {
        flush_pending();                         /* o lote usa essas texturas */
        frame_submit();
        for (int i = 0; i < TEX_SLOTS; i++)      /* cache cheio: recomeca */
            if (s_tex[i].used) img_destroy(&s_tex[i].img);
        memset(s_tex, 0, sizeof s_tex);
        s_tex_count = 0;
        s_tex_gen++;                             /* quem guardou views refaz */
        frame_begin();
        e = &s_tex[idx];
    }

    if (e->used && e->checked_frame == s_frame)
        return e->img.view;
    nbytes = tex_bytes(t);
    bytes = guest_bytes(va, nbytes);
    if (bytes)
        h = hash_bytes(bytes, nbytes);
    if (e->used && (h == e->hash || !bytes)) {
        e->checked_frame = s_frame;
        return e->img.view;
    }

    {
        size_t need = (size_t)t->width * t->height;
        if (need > s_decode_cap) {
            free(s_decode);
            s_decode = malloc(need * sizeof *s_decode);
            s_decode_cap = s_decode ? need : 0;
        }
        if (!s_decode || !nv2a_backend_decode_texture(t, s_decode)) {
            static int n;
            if (n++ < 12)
                fprintf(stderr, "[VK] decode falhou: off=%08X va=%08X %ux%u "
                        "pitch=%u fmt=0x%02X levels=%u\n", t->offset, va,
                        t->width, t->height, t->pitch, t->color, t->levels);
            if (!e->used)
                return s_white.view;
            e->checked_frame = s_frame;
            return e->img.view;
        }
    }
    if (!e->used) {
        if (!img_create(&e->img, t->width, t->height, COLOR_FMT,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT)) {
            img_destroy(&e->img);
            return s_white.view;
        }
        e->used = 1;
        e->offset = va; e->w = t->width; e->h = t->height; e->color = t->color;
        e->palette = pal_key;
        s_tex_count++;
        fresh = 1;
    }
    {
        if (dw4_frame_ring_on()) {
            /* Caixa-preta: toda textura criada ou cujo conteudo mudou neste quadro. */
            char ev[160];

            snprintf(ev, sizeof ev, "TEX %s va=%08X %ux%u f%02X pal=%08X:%08X hash %08X -> %08X",
                     fresh ? "NOVA" : "MUDOU", va, t->width, t->height, t->color, t->palette,
                     pal_key, (uint32_t)e->hash, (uint32_t)h);
            dw4_frame_detail(ev);
        }
        /* tex_upload pode enviar o frame (anel cheio), mas nunca esvazia o
         * cache: `e` continua valido. */
        tex_upload(&e->img, s_decode, t->width, t->height, fresh);
    }
    e->hash = h;
    e->checked_frame = s_frame;
    s_n_decode++;
    return e->img.view;
}

static int wrap_index(uint32_t nv)
{
    return nv == 1 ? 0 : (nv == 2 ? 1 : 2);     /* repeat, mirror, clamp */
}

static int filter_index(uint32_t f)
{
    uint32_t mag = (f >> 24) & 0xF, min = (f >> 16) & 0xFF;

    if (s_dbg & 16)
        return 0;
    return (mag == 2 || min == 2 || min == 4 || min == 6) ? 1 : 0;
}

static VkSampler sampler_get(int ws, int wt, int linear)
{
    static const VkSamplerAddressMode mode[3] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler *s = &s_samp[ws][wt][linear];

    if (!*s) {
        VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = mode[ws], .addressModeV = mode[wt],
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxAnisotropy = 1.0f, .maxLod = 0.0f };
        VKCHECK(vkCreateSampler(s_dev, &ci, NULL, s));
    }
    return *s;
}

/* ── Pipelines ─────────────────────────────────────────────────────────────
 * Tudo o que o Vulkan 1.0 fixa no pipeline. Normalizado (campos de um estado
 * desligado ficam em zero) para nao multiplicar pipelines a toa. */
typedef struct {
    uint8_t has_depth, has_ext, blend, depth_test, depth_write, stencil, bias, cmask;
    uint8_t cull, front_cw, pad[2];
    uint32_t bsrc, bdst, beq, dfunc, sfunc, sfail, szfail, szpass;
} PipeKey;

typedef struct {
    PipeKey key;
    VkPipeline pipe;
} PipeEntry;

#define MAX_PIPES 1024
static PipeEntry s_pipes[MAX_PIPES];
static int s_npipes;

static VkBlendFactor blend_factor(uint32_t nv, VkBlendFactor fallback)
{
    switch (nv) {
    case 0:      return VK_BLEND_FACTOR_ZERO;
    case 1:      return VK_BLEND_FACTOR_ONE;
    case 0x300:  return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306:  return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308:  return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return fallback;
    }
}

static VkBlendOp blend_op(uint32_t nv)
{
    switch (nv) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

/* GL_NEVER 0x200 .. GL_ALWAYS 0x207 estao na mesma ordem de VkCompareOp. */
static VkCompareOp compare_op(uint32_t nv)
{
    return (nv >= 0x200 && nv <= 0x207) ? (VkCompareOp)(nv - 0x200) : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp stencil_op(uint32_t nv)
{
    switch (nv) {
    case 0:      return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

static VkPipeline pipeline_get(const PipeKey *k)
{
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE };
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_fs, .pName = "main" } };
    /* Binding 1 (dados dos combiners): por vertice quando o lote tem ext; sem
     * ext, um unico elemento zerado lido por instancia. */
    VkVertexInputBindingDescription bind[2] = {
        { 0, sizeof(Nv2aVertex), VK_VERTEX_INPUT_RATE_VERTEX },
        { 1, sizeof(Nv2aVertexExt),
          k->has_ext ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE } };
    VkVertexInputAttributeDescription attr[9] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertex, x) },
        { 1, 0, VK_FORMAT_B8G8R8A8_UNORM,      offsetof(Nv2aVertex, diffuse) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Nv2aVertex, u) },
        { 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertexExt, tex) },
        { 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertexExt, tex) + 16 },
        { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertexExt, tex) + 32 },
        { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertexExt, tex) + 48 },
        { 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertexExt, specular) },
        { 8, 1, VK_FORMAT_R32_SFLOAT,          offsetof(Nv2aVertexExt, fog) } };
    VkPipelineVertexInputStateCreateInfo vin = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 2, .pVertexBindingDescriptions = bind,
        .vertexAttributeDescriptionCount = 9, .pVertexAttributeDescriptions = attr };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rast = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = s_depth_clamp ? VK_TRUE : VK_FALSE,   /* near ja recortado na CPU */
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = k->cull == 1 ? VK_CULL_MODE_FRONT_BIT
                  : k->cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = k->front_cw ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = k->bias, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkStencilOpState sop = { stencil_op(k->sfail), stencil_op(k->szpass),
                             stencil_op(k->szfail), compare_op(k->sfunc), 0xFF, 0xFF, 0 };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = k->depth_test, .depthWriteEnable = k->depth_write,
        .depthCompareOp = compare_op(k->dfunc), .stencilTestEnable = k->stencil,
        .front = sop, .back = sop, .maxDepthBounds = 1.0f };
    VkPipelineColorBlendAttachmentState att = {
        .blendEnable = k->blend,
        .srcColorBlendFactor = blend_factor(k->bsrc, VK_BLEND_FACTOR_ONE),
        .dstColorBlendFactor = blend_factor(k->bdst, VK_BLEND_FACTOR_ZERO),
        .colorBlendOp = blend_op(k->beq),
        .srcAlphaBlendFactor = blend_factor(k->bsrc, VK_BLEND_FACTOR_ONE),
        .dstAlphaBlendFactor = blend_factor(k->bdst, VK_BLEND_FACTOR_ZERO),
        .alphaBlendOp = blend_op(k->beq), .colorWriteMask = k->cmask };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof dyn / sizeof dyn[0], .pDynamicStates = dyn };
    VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin,
        .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rast,
        .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = s_layout, .renderPass = s_rp[k->has_depth] };
    VkPipeline pipe = VK_NULL_HANDLE;

    for (int i = 0; i < s_npipes; i++)
        if (memcmp(&s_pipes[i].key, k, sizeof *k) == 0)
            return s_pipes[i].pipe;
    if (s_npipes == MAX_PIPES) {
        LOG_ONCE("[VK] cache de pipelines cheio (%d)\n", MAX_PIPES);
        return VK_NULL_HANDLE;
    }
    if (vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe) != VK_SUCCESS) {
        LOG_ONCE("[VK] vkCreateGraphicsPipelines falhou\n");
        return VK_NULL_HANDLE;
    }
    s_pipes[s_npipes].key = *k;
    s_pipes[s_npipes++].pipe = pipe;
    return pipe;
}

/* ── Lotes ─────────────────────────────────────────────────────────────────── *
 * Tudo o que decide como um draw sai. Draws consecutivos com a mesma chave
 * sao acumulados e saem num unico vkCmdDraw. */
typedef struct {
    Surface *surf;
    Nv2aRenderState rs;
    int has_ext;
    VkImageView view[4];
    Surface *tsurf[4];                  /* estagio amostra uma superficie */
    uint32_t tva[4];                    /* endereco da textura no guest (diagnostico) */
    uint32_t tpal[4];                   /* endereco da paleta e resumo do conteudo dela */
    uint32_t tpalh[4];
    uint32_t tfmt[4];
    int wrap_s[4], wrap_t[4], filter[4];
    float tscale[4][2];
    float texsize[2];                   /* caminho simples */
    Nv2aCombiner rc;
    float fogc[4];
    uint32_t clip_mode;
    uint32_t sc[4];                     /* recorte do guest (x, y, l, a); zeros = inteira */
} DrawKey;

static DrawKey s_key;
static int s_key_valid;
static Nv2aVertex *s_pv;
static Nv2aVertexExt *s_pe;
static uint32_t s_pcount, s_pcap;

/* Sequencia de operacoes do quadro, para a caixa-preta: "c2" = clear na
 * superficie 2, "d0x3200" = 3200 draws seguidos na superficie 0, "t2" depois
 * de um draw = ele amostra a superficie 2 (render-to-texture). Comparar o
 * quadro defeituoso com os vizinhos mostra o que faltou ou trocou de ordem. */
/* Diagnostico por lote: matriz de textura do estagio 0 (registradores do
 * executor no momento de cada draw) -- hash combinado e faixa da translacao. */
extern const uint32_t *nv2a_pb_exec_reg_mirror(void);
static uint32_t s_tm_hash, s_tm_n;
static float s_tm_min[2], s_tm_max[2];

static void tm_note(void)
{
    const uint32_t *reg = nv2a_pb_exec_reg_mirror();
    float m[16];
    uint32_t h = 0;

    if (!reg[0x0420 / 4])
        return;
    memcpy(m, &reg[0x06C0 / 4], sizeof m);
    for (int i = 0; i < 16; i++)
        h = h * 31u + reg[0x06C0 / 4 + i];
    s_tm_hash = (s_tm_hash << 5 | s_tm_hash >> 27) ^ h;
    for (int a = 0; a < 2; a++) {
        float t = m[a * 4 + 3];
        if (!s_tm_n || t < s_tm_min[a]) s_tm_min[a] = t;
        if (!s_tm_n || t > s_tm_max[a]) s_tm_max[a] = t;
    }
    s_tm_n++;
}

static char s_seq[224];
static int s_seq_len, s_seq_surf = -1, s_seq_run, s_seq_tex = -1;

static void seq_flush_run(void)
{
    if (s_seq_run && s_seq_len < (int)sizeof s_seq - 24) {
        s_seq_len += snprintf(s_seq + s_seq_len, sizeof s_seq - (size_t)s_seq_len,
                              s_seq_tex >= 0 ? "d%dx%dt%d " : "d%dx%d ",
                              s_seq_surf, s_seq_run, s_seq_tex);
    }
    s_seq_run = 0;
}

static void seq_clear(const Surface *e, uint32_t flags, uint32_t argb)
{
    seq_flush_run();
    if (s_seq_len < (int)sizeof s_seq - 24)
        s_seq_len += snprintf(s_seq + s_seq_len, sizeof s_seq - (size_t)s_seq_len,
                              "c%d:%02X:%06X ", (int)(e - s_surf), flags, argb & 0xFFFFFF);
    s_seq_surf = -1;
}

static void seq_draw(const Surface *e, const Surface *sampled)
{
    int si = (int)(e - s_surf), ti = sampled ? (int)(sampled - s_surf) : -1;

    if (si != s_seq_surf || ti != s_seq_tex) {
        seq_flush_run();
        s_seq_surf = si;
        s_seq_tex = ti;
    }
    s_seq_run++;
}

static float zmax_of(const Nv2aRenderState *rs)
{
    return rs->depth_max > 65536.0f ? 16777215.0f : 65535.0f;
}

static VkDescriptorSet set_get(const DrawKey *k, const VkSampler samp[4])
{
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s_dpool, .descriptorSetCount = 1, .pSetLayouts = &s_dsl };
    VkDescriptorBufferInfo bi = { s_ubo.buf, 0, sizeof(Ubo) };
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet wr[5];
    uint64_t h = 0x9E3779B97F4A7C15ull;
    SetEntry *c;

    for (int i = 0; i < 4; i++) {
        h = (h ^ (uint64_t)(uintptr_t)k->view[i]) * 0x100000001B3ull;
        h = (h ^ (uint64_t)(uintptr_t)samp[i]) * 0x100000001B3ull;
        h ^= h >> 29;
    }
    c = &s_setc[h % SET_CACHE];
    if (c->set && memcmp(c->view, k->view, sizeof c->view) == 0
            && memcmp(c->samp, samp, sizeof c->samp) == 0)
        return c->set;

    if (vkAllocateDescriptorSets(s_dev, &ai, &c->set) != VK_SUCCESS) {
        c->set = VK_NULL_HANDLE;
        s_lost = 1;
        return VK_NULL_HANDLE;
    }
    s_sets_used++;
    memcpy(c->view, k->view, sizeof c->view);
    memcpy(c->samp, samp, sizeof c->samp);
    memset(wr, 0, sizeof wr);
    wr[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[0].dstSet = c->set;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    wr[0].pBufferInfo = &bi;
    for (int i = 0; i < 4; i++) {
        ii[i].sampler = samp[i];
        ii[i].imageView = k->view[i];
        ii[i].imageLayout = k->tsurf[i] ? VK_IMAGE_LAYOUT_GENERAL
                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[1 + i].dstSet = c->set;
        wr[1 + i].dstBinding = 1 + (uint32_t)i;
        wr[1 + i].descriptorCount = 1;
        wr[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[1 + i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s_dev, 5, wr, 0, NULL);
    return c->set;
}

static void flush_pending(void)
{
    const DrawKey *k = &s_key;
    const Nv2aRenderState *rs = &k->rs;
    VkDeviceSize zero = 0;
    VkSampler samp[4];
    VkDescriptorSet set;
    VkPipeline pipe;
    PipeKey pk;
    Zeta *zeta = NULL;
    Ubo u;
    float bc[4];

    if (!s_pcount || !s_key_valid || s_lost) {
        s_pcount = 0;
        return;
    }
    /* 16:9: comprime em x, com o lote inteiro a vista (render_opts.h). */
    if (k->surf->wide)
        ro_squeeze(s_pv, s_pcount, (float)k->surf->w, (float)ro_display_rows(k->surf->h),
                   k->view[0] != s_white.view || k->view[1] != s_white.view
                   || k->view[2] != s_white.view || k->view[3] != s_white.view,
                   (k->tsurf[0] && k->tsurf[0]->wide) || (k->tsurf[1] && k->tsurf[1]->wide)
                   || (k->tsurf[2] && k->tsurf[2]->wide) || (k->tsurf[3] && k->tsurf[3]->wide),
                   k->sc);
    frame_begin();
    if (s_vtx_used + s_pcount > VTX_CAP || s_ubo_used + s_ubo_step > UBO_CAP
            || s_sets_used >= MAX_SETS) {
        frame_submit();                 /* aneis cheios: envia e continua */
        frame_begin();
    }
    if (rs->zeta_va)
        zeta = zeta_get(rs->zeta_va, k->surf);

    /* Uma superficie desenhada e depois amostrada: a escrita precisa estar
     * visivel para a leitura (fora de qualquer render pass). */
    for (int i = 0; i < 4; i++) {
        Surface *ts = k->tsurf[i];
        if (ts && ts->dirty) {
            end_pass();
            barrier(s_cmd, ts->color.img, VK_IMAGE_ASPECT_COLOR_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ts->dirty = 0;
        }
    }
    if (!begin_pass(k->surf, zeta) || s_lost) {
        s_pcount = 0;
        return;
    }
    k->surf->dirty = 1;
    {
        /* Recorte do guest (begin_pass deixa o da superficie inteira). */
        VkRect2D sr = { { 0, 0 }, { k->surf->pw, k->surf->ph } };
        int32_t r[4];

        if (ro_scissor(k->sc, k->surf->wide, k->surf->w, k->surf->h, k->surf->pw,
                       k->surf->ph, r)) {
            sr.offset.x = r[0]; sr.offset.y = r[1];
            sr.extent.width = (uint32_t)r[2]; sr.extent.height = (uint32_t)r[3];
        }
        vkCmdSetScissor(s_cmd, 0, 1, &sr);
    }

    /* ── pipeline ── */
    memset(&pk, 0, sizeof pk);
    pk.has_depth = zeta != NULL;
    pk.has_ext = (uint8_t)k->has_ext;
    if (rs->blend_enable && !(s_dbg & 2)) {
        pk.blend = 1;
        pk.bsrc = rs->blend_src; pk.bdst = rs->blend_dst; pk.beq = rs->blend_eq;
    }
    if (zeta && (rs->depth_test_enable || rs->depth_write) && !(s_dbg & 4)) {
        pk.depth_test = 1;
        pk.dfunc = rs->depth_test_enable ? rs->depth_func : 0x207;
        pk.depth_write = rs->depth_write != 0;
    }
    if (zeta && rs->stencil_enable) {
        pk.stencil = 1;
        pk.sfunc = rs->stencil_func; pk.sfail = rs->stencil_fail;
        pk.szfail = rs->stencil_zfail; pk.szpass = rs->stencil_zpass;
    }
    pk.cmask = (uint8_t)(((rs->color_mask & 0x00010000) ? VK_COLOR_COMPONENT_R_BIT : 0)
                       | ((rs->color_mask & 0x00000100) ? VK_COLOR_COMPONENT_G_BIT : 0)
                       | ((rs->color_mask & 0x00000001) ? VK_COLOR_COMPONENT_B_BIT : 0)
                       | ((rs->color_mask & 0x01000000) ? VK_COLOR_COMPONENT_A_BIT : 0));
    pk.bias = rs->poly_offset_enable && !s_no_poly_offset
           && rs->poly_offset_factor == rs->poly_offset_factor
           && rs->poly_offset_bias == rs->poly_offset_bias;
    if (s_cull_mode && rs->cull_enable
            && (rs->cull_face == 0x404 || rs->cull_face == 0x405)
            && (rs->front_face == 0x900 || rs->front_face == 0x901)) {
        /* A NV2A define horario/anti-horario na tela com y para baixo, que e
         * a convencao do framebuffer do Vulkan: o valor vale direto. Depende
         * de RECOMP_STRIP_ALT_WINDING=1 (ver nv2a_gl_backend.c). */
        pk.cull = rs->cull_face == 0x404 ? 1 : 2;
        pk.front_cw = rs->front_face == 0x900;
        if (s_cull_mode == 2)
            pk.front_cw = !pk.front_cw;
    }
    pipe = pipeline_get(&pk);
    if (!pipe) {
        s_pcount = 0;
        return;
    }
    if (pipe != s_bound_pipe) {
        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        s_bound_pipe = pipe;
    }
    bc[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bc[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bc[2] = (rs->blend_color & 0xFF) / 255.0f;
    bc[3] = (rs->blend_color >> 24) / 255.0f;
    vkCmdSetBlendConstants(s_cmd, bc);
    /* z-bias: (fator, unidades) do glPolygonOffset = (slope, constant) aqui. */
    vkCmdSetDepthBias(s_cmd, pk.bias ? rs->poly_offset_bias : 0.0f, 0.0f,
                      pk.bias ? rs->poly_offset_factor : 0.0f);
    vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FRONT_AND_BACK, rs->stencil_func_mask);
    vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FRONT_AND_BACK, rs->stencil_write_mask);
    vkCmdSetStencilReference(s_cmd, VK_STENCIL_FRONT_AND_BACK, rs->stencil_ref);

    /* ── uniformes ── */
    memset(&u, 0, sizeof u);
    u.surf[0] = (float)k->surf->w; u.surf[1] = (float)k->surf->h; u.surf[2] = zmax_of(rs);
    u.texsize[0] = k->texsize[0]; u.texsize[1] = k->texsize[1];
    u.alpha[0] = (rs->alpha_test_enable && !(s_dbg & 1)) ? (int32_t)rs->alpha_func : 0;
    u.alpha[1] = (int32_t)rs->alpha_ref;
    u.alpha[2] = (s_dbg & 8) ? 0 : k->has_ext;
    if (k->has_ext) {
        const Nv2aCombiner *rc = &k->rc;

        for (int i = 0; i < 4; i++) {
            u.tscale[i][0] = k->tscale[i][0];
            u.tscale[i][1] = k->tscale[i][1];
        }
        memcpy(u.fogc, k->fogc, sizeof u.fogc);
        for (int i = 0; i < 8; i++) {
            u.cw[i][0] = rc->color_icw[i]; u.cw[i][1] = rc->alpha_icw[i];
            u.cw[i][2] = rc->color_ocw[i]; u.cw[i][3] = rc->alpha_ocw[i];
            nv2a_rc_unpack(rc->factor0[i], u.f0[i]);
            nv2a_rc_unpack(rc->factor1[i], u.f1[i]);
        }
        nv2a_rc_unpack(rc->final_c0, u.fc0);
        nv2a_rc_unpack(rc->final_c1, u.fc1);
        u.fin[0] = rc->final0; u.fin[1] = rc->final1;
        u.ctl[0] = rc->control; u.ctl[1] = rc->stage_program; u.ctl[2] = k->clip_mode;
    }
    if (!s_last_ubo_valid || memcmp(&u, &s_last_ubo, sizeof u) != 0) {
        memcpy((uint8_t *)s_ubo.map + s_ubo_used, &u, sizeof u);
        s_last_ubo = u;
        s_last_ubo_off = s_ubo_used;
        s_last_ubo_valid = 1;
        s_ubo_used += s_ubo_step;
    }

    /* ── texturas ── */
    for (int i = 0; i < 4; i++)
        samp[i] = k->view[i] == s_white.view ? sampler_get(0, 0, 0)
                : sampler_get(k->wrap_s[i], k->wrap_t[i], k->filter[i]);
    set = set_get(k, samp);
    if (!set) {
        s_pcount = 0;
        return;
    }
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_layout, 0, 1, &set,
                            1, &s_last_ubo_off);

    /* ── vertices ── */
    memcpy((uint8_t *)s_vb.map + (size_t)s_vtx_used * sizeof(Nv2aVertex), s_pv,
           (size_t)s_pcount * sizeof(Nv2aVertex));
    if (k->has_ext)
        memcpy((uint8_t *)s_vbe.map + (size_t)s_vtx_used * sizeof(Nv2aVertexExt), s_pe,
               (size_t)s_pcount * sizeof(Nv2aVertexExt));
    if (s_bound_ext < 0)
        vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_vb.buf, &zero);
    if (s_bound_ext != k->has_ext) {
        vkCmdBindVertexBuffers(s_cmd, 1, 1, k->has_ext ? &s_vbe.buf : &s_dummy.buf, &zero);
        s_bound_ext = k->has_ext;
    }
    if (dw4_frame_ring_on()) {
        /* Caixa-preta: o que este lote e, para comparar quadro a quadro. */
        char line[480];
        uint32_t rch = 0;

        for (size_t i = 0; i < sizeof k->rc / 4; i++)
            rch = rch * 31u + ((const uint32_t *)&k->rc)[i];
        uint32_t sum = 0, shash = 0, thash = 0;
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        double fog = 0.0, fmin = 1e9, fmax = -1e9;

        if (k->has_ext) {               /* neblina, especular e coordenadas de textura */
            for (uint32_t i = 0; i < s_pcount; i++) {
                const Nv2aVertexExt *x = &s_pe[i];
                uint32_t w[2];

                fog += x->fog;
                if (x->fog < fmin) fmin = x->fog;
                if (x->fog > fmax) fmax = x->fog;
                memcpy(w, x->specular, 8);
                shash = shash * 31u + w[0] + w[1];
                memcpy(w, x->tex[0], 8);
                thash = thash * 31u + w[0] + w[1];
            }
            fog /= s_pcount;
        }

        for (uint32_t i = 0; i < s_pcount; i++) {
            const Nv2aVertex *v = &s_pv[i];
            sum = sum * 31u + v->diffuse;
            if (v->x < x0) x0 = v->x;
            if (v->x > x1) x1 = v->x;
            if (v->y < y0) y0 = v->y;
            if (v->y > y1) y1 = v->y;
        }
        snprintf(line, sizeof line, "s%d v=%u ext=%d bl=%u(%X,%X) at=%u(%X,%u) z=%u/%u cm=%08X "
                 "t=%08X,%08X f%02X pal=%08X:%08X ts=%d rc=%05X f0=%08X fin=%X,%X fogc=%.2f,%.2f,%.2f "
                 "fog=%.3f[%.2f..%.2f] spec=%08X uv=%08X col=%08X bb=%.0f,%.0f-%.0f,%.0f "
                 "rch=%08X f1=%08X fc=%08X,%08X bc=%08X tm=%u:%08X[%.3f..%.3f,%.3f..%.3f]",
                 (int)(k->surf - s_surf), s_pcount, k->has_ext, rs->blend_enable, rs->blend_src,
                 rs->blend_dst, rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
                 rs->depth_test_enable, rs->depth_write, rs->color_mask, k->tva[0], k->tva[1],
                 k->tfmt[0], k->tpal[0], k->tpalh[0],
                 k->tsurf[0] ? (int)(k->tsurf[0] - s_surf) : -1, k->rc.stage_program,
                 k->rc.factor0[0], k->rc.final0, k->rc.final1, (double)k->fogc[0],
                 (double)k->fogc[1], (double)k->fogc[2], fog, k->has_ext ? fmin : 0.0,
                 k->has_ext ? fmax : 0.0, shash, thash, sum,
                 (double)x0, (double)y0, (double)x1, (double)y1,
                 rch, k->rc.factor1[0], k->rc.final_c0, k->rc.final_c1, rs->blend_color,
                 s_tm_n, s_tm_hash, (double)s_tm_min[0], (double)s_tm_max[0],
                 (double)s_tm_min[1], (double)s_tm_max[1]);
        s_tm_hash = 0; s_tm_n = 0;
        dw4_frame_detail(line);
    }
    vkCmdDraw(s_cmd, s_pcount, 1, s_vtx_used, 0);
    s_vtx_used += s_pcount;
    s_n_flush++;
    s_pcount = 0;
}

/* Poe um lote na fila (funde com o anterior se a chave for a mesma). */
static void enqueue(const DrawKey *key, const Nv2aVertex *v, const Nv2aVertexExt *x,
                    uint32_t count)
{
    if (count > PENDING_MAX)
        count = PENDING_MAX - PENDING_MAX % 3;
    if (!s_key_valid || memcmp(key, &s_key, sizeof *key) != 0
            || s_pcount + count > PENDING_MAX) {
        flush_pending();
        s_key = *key;
        s_key_valid = 1;
    }
    if (s_pcount + count > s_pcap) {
        uint32_t cap = s_pcap ? s_pcap : 16384;
        while (cap < s_pcount + count)
            cap *= 2;
        s_pv = realloc(s_pv, cap * sizeof *s_pv);
        s_pe = realloc(s_pe, cap * sizeof *s_pe);
        s_pcap = (s_pv && s_pe) ? cap : 0;
        if (!s_pcap) {
            s_pcount = 0;
            return;
        }
    }
    memcpy(s_pv + s_pcount, v, count * sizeof *s_pv);
    if (x)
        memcpy(s_pe + s_pcount, x, count * sizeof *s_pe);
    s_pcount += count;
}

static void key_init(DrawKey *key, Surface *e, const Nv2aRenderState *rs)
{
    memset(key, 0, sizeof *key);
    key->surf = e;
    key->rs = *rs;
    key->texsize[0] = key->texsize[1] = 1.0f;
    for (int i = 0; i < 4; i++) {
        key->view[i] = s_white.view;
        key->tscale[i][0] = key->tscale[i][1] = 1.0f;
    }
}

/* ── Callbacks ─────────────────────────────────────────────────────────────── */

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkClearAttachment att[2];
    VkClearRect rect = { .layerCount = 1 };
    uint32_t n = 0;
    Surface *e;
    Zeta *zeta = NULL;

    if (!s_ok || s_lost)
        return;
    flush_pending();
    frame_begin();
    e = surface_get(s);
    if (!e)
        return;
    if (rs->zeta_va)
        zeta = zeta_get(rs->zeta_va, e);
    rect.rect.extent.width = e->pw;
    rect.rect.extent.height = e->ph;
    seq_clear(e, flags, argb);

    if ((flags & 0xF0) == 0xF0) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        att[n].clearValue.color.float32[0] = ((argb >> 16) & 0xFF) / 255.0f;
        att[n].clearValue.color.float32[1] = ((argb >> 8) & 0xFF) / 255.0f;
        att[n].clearValue.color.float32[2] = (argb & 0xFF) / 255.0f;
        att[n].clearValue.color.float32[3] = (argb >> 24) / 255.0f;
        n++;
    } else if (flags & 0xF0) {
        /* Clear de alguns canais so: vkCmdClearAttachments ignora a mascara de
         * escrita, entao sai como um retangulo com a mascara no pipeline. */
        Nv2aRenderState fill;
        Nv2aVertex q[6];
        DrawKey key;
        float w = (float)e->w, h = (float)e->h;
        const float xy[6][2] = { {0, 0}, {w, 0}, {w, h}, {0, 0}, {w, h}, {0, h} };

        memset(&fill, 0, sizeof fill);
        fill.color_mask = ((flags & 0x10) ? 0x00010000u : 0) | ((flags & 0x20) ? 0x00000100u : 0)
                        | ((flags & 0x40) ? 0x00000001u : 0) | ((flags & 0x80) ? 0x01000000u : 0);
        fill.zeta_va = rs->zeta_va;
        fill.depth_max = rs->depth_max;
        memset(q, 0, sizeof q);
        for (int i = 0; i < 6; i++) {
            q[i].x = xy[i][0]; q[i].y = xy[i][1]; q[i].rhw = 1.0f; q[i].diffuse = argb;
        }
        key_init(&key, e, &fill);
        enqueue(&key, q, NULL, 6);
        flush_pending();
    }
    if (flags & 0xF0)
        note_shown(e);
    if (zeta && (flags & 0x3)) {
        att[n].aspectMask = ((flags & 0x1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                          | ((flags & 0x2) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        att[n].colorAttachment = 0;
        att[n].clearValue.depthStencil.depth =
            zmax_of(rs) > 65536.0f ? (float)(zstencil >> 8) / 16777215.0f
                                   : (float)(zstencil & 0xFFFF) / 65535.0f;
        att[n].clearValue.depthStencil.stencil = zstencil & 0xFF;
        n++;
    }
    if (!n || s_lost || !begin_pass(e, zeta))
        return;
    e->dirty = 1;
    vkCmdClearAttachments(s_cmd, n, att, 1, &rect);
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    DrawKey key;
    Surface *e;
    unsigned gen;

    if (!s_ok || s_lost || !b->count)
        return;
    if (s_skip_depth_only && !(b->state->color_mask & 0x01010101u))
        return;
    frame_begin();
    /* A superficie pode ser recriada aqui; o lote pendente aponta para ela
     * (surface_free o envia antes). */
    e = surface_get(s);
    if (!e)
        return;

    do {
        /* Se o cache de texturas for esvaziado no meio (texture_get), as
         * views ja guardadas nesta chave morreram: resolve tudo de novo. */
        gen = s_tex_gen;
        key_init(&key, e, b->state);
        /* Recorte menor que a superficie (sub-viewport: o minimapa). */
        if (s->width && s->height && (s->width < e->w || s->height < e->h)) {
            key.sc[0] = s->clip_x; key.sc[1] = s->clip_y;
            key.sc[2] = s->width;  key.sc[3] = s->height;
        }
        if (b->ext) {
            const Nv2aBatchExt *x = b->ext;

            key.has_ext = 1;
            key.rc = x->rc;
            memcpy(key.fogc, x->fog_color, sizeof key.fogc);
            key.clip_mode = x->clip_plane_mode;
            for (int i = 0; i < 4; i++) {
                const Nv2aTexture *t = &x->tex[i];
                uint32_t mode = (x->rc.stage_program >> (i * 5)) & 0x1F;

                if (!(mode >= 1 && mode <= 3) || !x->tex_valid[i])
                    continue;           /* estagio sem textura: le branco */
                key.view[i] = texture_get(t, e, &key.tsurf[i]);
                if (key.view[i] == s_white.view)
                    continue;
                key.tva[i] = tex_va(t->offset);
                key.tfmt[i] = t->color;
                key.tpal[i] = t->palette;
                if (t->palette && dw4_frame_ring_on()) {     /* so diagnostico */
                    const uint8_t *pal = guest_bytes(t->palette, 1024);
                    uint64_t ph = pal ? hash_bytes(pal, 1024) : 0;
                    key.tpalh[i] = (uint32_t)(ph ^ (ph >> 32));
                }
                key.wrap_s[i] = wrap_index(t->addr_u);
                key.wrap_t[i] = wrap_index(t->addr_v);
                key.filter[i] = filter_index(t->filter);
                if (!x->tex_normalised[i]) {
                    key.tscale[i][0] = 1.0f / (float)t->width;
                    key.tscale[i][1] = 1.0f / (float)t->height;
                }
            }
        } else if (b->texture) {
            key.view[0] = texture_get(b->texture, e, &key.tsurf[0]);
            if (key.view[0] != s_white.view) {
                key.tva[0] = tex_va(b->texture->offset);
                key.tfmt[0] = b->texture->color;
                key.tpal[0] = b->texture->palette;
                key.texsize[0] = (float)b->texture->width;
                key.texsize[1] = (float)b->texture->height;
                key.wrap_s[0] = wrap_index(b->texture->addr_u);
                key.wrap_t[0] = wrap_index(b->texture->addr_v);
                key.filter[0] = filter_index(b->texture->filter);
            }
        }
    } while (gen != s_tex_gen);
    if (s_lost)
        return;

    enqueue(&key, b->vertices, b->ext ? b->ext->vertices : NULL, b->count);
    if (b->ext && dw4_frame_ring_on())
        tm_note();
    seq_draw(e, key.tsurf[0] ? key.tsurf[0] : key.tsurf[1] ? key.tsurf[1]
                : key.tsurf[2] ? key.tsurf[2] : key.tsurf[3]);
    note_shown(e);
    s_n_draw++;
    s_n_tris += b->count / 3;
}

/* Janela do toolkit: recebe um frame de qualquer tamanho (patch 4.10). */
extern void xbox_FramebufferWindowPresentPixels(const void *pixels, uint32_t w,
                                                uint32_t h, uint32_t pitch);

static uint32_t s_last_va, s_last_pitch;    /* ultimo frame trazido ao guest */
static int s_f9_pending;                    /* F9 no modo direto: grava no proximo frame */
static void f9_save(void);

/* Obtem a proxima imagem do swapchain (refazendo-o se a janela mudou).
 * 0 = este frame nao vai para a janela. */
static int swap_acquire(void)
{
    FrameSet *f = &s_sets[s_set];
    int dw = 0, dh = 0;

    if (!s_swap.surf || s_swap.fails > 8)
        return 0;
    xbox_FramebufferWindowVkDrawableSize(&dw, &dh);
    if (dw <= 0 || dh <= 0)
        return 0;                       /* minimizada */
    if (s_swap.ok && ((uint32_t)dw != s_swap.w || (uint32_t)dh != s_swap.h))
        s_swap.stale = 1;               /* Wayland nao avisa: o tamanho e nosso */
    for (int attempt = 0; attempt < 2; attempt++) {
        VkResult r;

        if (!s_swap.ok || s_swap.stale) {
            sets_wait_all();            /* nada em voo usa as imagens antigas */
            if (!swap_create()) {
                s_swap.fails++;
                return 0;
            }
        }
        r = vkAcquireNextImageKHR(s_dev, s_swap.sc, UINT64_MAX, f->acquired,
                                  VK_NULL_HANDLE, &s_swap.acquired);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            if (r == VK_SUBOPTIMAL_KHR)
                s_swap.stale = 1;       /* serve para este frame; refaz depois */
            s_swap.fails = 0;
            return 1;
        }
        s_swap.acquired = SWAP_NONE;
        if (r != VK_ERROR_OUT_OF_DATE_KHR) {
            fprintf(stderr, "[VK] vkAcquireNextImageKHR falhou (%d)\n", (int)r);
            s_swap.fails++;
            s_swap.ok = 0;
            return 0;
        }
        s_swap.stale = 1;
    }
    return 0;
}

/* Modo direto: o frame pronto vai da superficie para a imagem do swapchain
 * com um blit (escala e faixas pretas feitas pela GPU) e o envio nao espera.
 * So le o frame de volta para a CPU se alguem pediu pixels. */
static void present_direct(Surface *e)
{
    static const VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
    static const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    uint32_t src_w = e->pw;
    uint32_t src_h = (uint32_t)((float)ro_display_rows(e->h) * ro_scale + 0.5f);
    int want = dw4_frame_wanted() || s_f9_pending;
    int shown, from_swap = 0;
    VkImage dst = VK_NULL_HANDLE;

    if (src_h > e->ph) src_h = e->ph;
    if (!src_w || !src_h) {
        frame_submit_ex(1);
        return;
    }
    if (want) {
        VkDeviceSize need = (VkDeviceSize)src_w * src_h * 4;

        if (s_swap.shot && need < (VkDeviceSize)s_swap.w * s_swap.h * 4)
            need = (VkDeviceSize)s_swap.w * s_swap.h * 4;

        if (need > s_rb.size) {
            frame_submit();             /* nada na GPU usa o buffer antigo */
            dw4_frame_note_host(NULL, 0, 0, 0);
            buf_destroy(&s_rb);
            s_buf_cached = 1;
            if (!buf_create(&s_rb, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
                s_lost = 1;
                return;
            }
        }
    }
    frame_begin();
    end_pass();
    shown = swap_acquire();
    if (s_lost)
        return;
    frame_begin();                      /* swap_acquire pode ter esperado a GPU */
    barrier(s_cmd, e->color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    e->dirty = 0;
    if (shown) {
        VkImageBlit blit = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 } };
        /* Maior retangulo com a forma do frame, centrado na janela. */
        uint32_t dw = s_swap.w, dh = (uint32_t)((uint64_t)s_swap.w * src_h / src_w);

        if (dh > s_swap.h) {
            dh = s_swap.h;
            dw = (uint32_t)((uint64_t)s_swap.h * src_w / src_h);
        }
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        dst = s_swap.img[s_swap.acquired];
        from_swap = want && s_swap.shot
                 && (VkDeviceSize)s_swap.w * s_swap.h * 4 <= s_rb.size;
        blit.srcOffsets[1] = (VkOffset3D){ (int32_t)src_w, (int32_t)src_h, 1 };
        blit.dstOffsets[0] = (VkOffset3D){ (int32_t)((s_swap.w - dw) / 2),
                                           (int32_t)((s_swap.h - dh) / 2), 0 };
        blit.dstOffsets[1] = (VkOffset3D){ blit.dstOffsets[0].x + (int32_t)dw,
                                           blit.dstOffsets[0].y + (int32_t)dh, 1 };
        barrier(s_cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (dw != s_swap.w || dh != s_swap.h)
            vkCmdClearColorImage(s_cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1,
                                 &range);
        /* 640x480 original ampliado: pixels nitidos, como a janela SDL fazia;
         * imagem em resolucao interna maior: filtro linear. */
        vkCmdBlitImage(s_cmd, e->color.img, VK_IMAGE_LAYOUT_GENERAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       ro_enhanced() ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        if (from_swap) {
            VkBufferImageCopy region = {
                .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .imageExtent = { s_swap.w, s_swap.h, 1 } };

            barrier(s_cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyImageToBuffer(s_cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_rb.buf,
                                   1, &region);
        }
        barrier(s_cmd, dst, VK_IMAGE_ASPECT_COLOR_BIT,
                from_swap ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                          : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                from_swap ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        xbox_FramebufferWindowFrameShape(src_w, src_h);
    }
    if (want) {
        VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                     .imageExtent = { src_w, src_h, 1 } };
        uint32_t out_w = from_swap ? s_swap.w : src_w, out_h = from_swap ? s_swap.h : src_h;

        if (!from_swap)
            vkCmdCopyImageToBuffer(s_cmd, e->color.img, VK_IMAGE_LAYOUT_GENERAL, s_rb.buf, 1,
                                   &region);
        frame_submit();                 /* espera: os pixels sao lidos ja */
        if (s_lost)
            return;
        s_n_readback++;
        dw4_frame_note_host(s_rb.map, out_w, out_h, out_w * 4);
        if (s_f9_pending) {
            s_f9_pending = 0;
            f9_save();
        }
    } else {
        frame_submit_ex(1);
    }
    if (e->w != 640)
        s_n_odd_present++;
    s_last_va = 0;
}

/* Antes do snapshot da janela: traz o frame pronto para a memoria do guest
 * (modo leitura) ou o apresenta pelo swapchain (modo direto). */
static void be_present_sync(void)
{
    VkBufferImageCopy region = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 } };
    Surface *e;
    uint8_t *dst;
    const uint8_t *src;
    uint32_t rows;
    VkDeviceSize need;

    if (!s_ok || s_lost)
        return;
    flush_pending();
    e = s_shown;
    if (!e || !e->color.img) {
        frame_submit();
        return;
    }
    rows = e->clip_h && e->clip_h < e->h ? e->clip_h : e->h;
    if (s_async) {
        present_direct(e);
        return;
    }
    if (ro_enhanced()) {
        /* Resolucao interna / widescreen: o frame nao cabe nos 640x480 do
         * guest. Vai direto para a janela e para as capturas. */
        uint32_t prow = (uint32_t)((float)ro_display_rows(e->h) * ro_scale + 0.5f);

        if (prow > e->ph) prow = e->ph;
        need = (VkDeviceSize)e->pw * prow * 4;
        if (need > s_rb.size) {
            frame_submit();
            dw4_frame_note_host(NULL, 0, 0, 0);
            buf_destroy(&s_rb);
            s_buf_cached = 1;
            if (!buf_create(&s_rb, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
                s_lost = 1;
                return;
            }
        }
        frame_begin();
        end_pass();
        barrier(s_cmd, e->color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        region.imageExtent = (VkExtent3D){ e->pw, prow, 1 };
        vkCmdCopyImageToBuffer(s_cmd, e->color.img, VK_IMAGE_LAYOUT_GENERAL, s_rb.buf, 1,
                               &region);
        frame_submit();
        if (s_lost)
            return;
        {
            double t0 = now_s();

            xbox_FramebufferWindowPresentPixels(s_rb.map, e->pw, prow, e->pw * 4);
            dw4_frame_note_host(s_rb.map, e->pw, prow, e->pw * 4);
            s_t_copy += now_s() - t0;
        }
        if (e->w != 640) {
            static int n;
            s_n_odd_present++;
            if (n++ < 20)
                fprintf(stderr, "[VK] f%u: exibida a superficie %08X %ux%u (nao e a tela)\n",
                        s_frame, e->va, e->w, e->h);
        }
        s_last_va = 0;
        return;
    }
    if (e->va >= CONTIG_BASE) {
        if ((uint64_t)e->va + (uint64_t)e->pitch * rows
                > (uint64_t)CONTIG_BASE + CONTIG_SIZE)
            goto submit_only;
    } else if ((uint64_t)e->va + (uint64_t)e->pitch * rows > xbox_GetMappedSize()) {
        goto submit_only;
    }
    if (e->pitch < e->w * e->bpp)
        goto submit_only;
    need = (VkDeviceSize)e->w * rows * 4;
    if (need > s_rb.size) {
        frame_submit();                 /* nada na GPU usa o buffer antigo */
        buf_destroy(&s_rb);
        if (!buf_create(&s_rb, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            s_lost = 1;
            return;
        }
    }
    frame_begin();
    end_pass();
    barrier(s_cmd, e->color.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    region.imageExtent = (VkExtent3D){ e->w, rows, 1 };
    vkCmdCopyImageToBuffer(s_cmd, e->color.img, VK_IMAGE_LAYOUT_GENERAL, s_rb.buf, 1, &region);
    frame_submit();
    if (s_lost)
        return;

    dst = (uint8_t *)xbox_GetMemoryOffset() + e->va;
    src = s_rb.map;
    if (e->bpp == 4) {
        for (uint32_t y = 0; y < rows; y++)
            memcpy(dst + (size_t)y * e->pitch, src + (size_t)y * e->w * 4, (size_t)e->w * 4);
    } else {                                    /* R5G6B5 */
        for (uint32_t y = 0; y < rows; y++) {
            const uint8_t *p = src + (size_t)y * e->w * 4;
            uint16_t *d = (uint16_t *)(dst + (size_t)y * e->pitch);
            for (uint32_t x = 0; x < e->w; x++, p += 4)
                d[x] = (uint16_t)(((p[2] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[0] >> 3));
        }
    }
    s_last_va = e->va;
    s_last_pitch = e->bpp == 4 ? e->pitch : 0;
    return;

submit_only:
    frame_submit();
}

/* ── Teclas de diagnostico (janela do jogo em foco) ────────────────────────
 * As mesmas do back end OpenGL, menos a captura F9. */
extern int xbox_FramebufferKeyDown(int vk);

static void f9_save(void)
{
    const char *dir = getenv("DW4_SHOT_DIR");
    char path[512];
    int seq = 0;

    if (!dir || !*dir)
        dir = "build/shots";
    for (; seq < 1000; seq++) {
        FILE *t;
        snprintf(path, sizeof path, "%s/f9-%03d.bmp", dir, seq);
        t = fopen(path, "r");
        if (!t)
            break;
        fclose(t);
    }
    fprintf(stderr, "[VK] F9: %s %s (rastreio de draws: so no OpenGL)\n",
            dw4_screenshot_save_now(path) ? "captura em" : "NAO gravou", path);
}

static void hotkeys_poll(void)
{
    static int prev[8];
    static const char *cull_name[3] = { "DESLIGADO", "normal", "INVERTIDO" };

    for (int i = 0; i < 8; i++) {
        int down = xbox_FramebufferKeyDown(0x74 + i);      /* VK_F5.. */
        int edge = down && !prev[i];

        prev[i] = down;
        if (!edge)
            continue;
        switch (i) {
        case 0:
            s_cull_mode = s_cull_mode == 1 ? 0 : (s_cull_mode == 0 ? 2 : 1);
            fprintf(stderr, "[VK] F5: culling %s\n", cull_name[s_cull_mode]);
            break;
        case 1:
            s_dbg ^= 1;
            fprintf(stderr, "[VK] F6: teste de alfa %s\n", (s_dbg & 1) ? "DESLIGADO" : "normal");
            break;
        case 2:
            s_dbg ^= 4;
            fprintf(stderr, "[VK] F7: teste de profundidade %s\n", (s_dbg & 4) ? "DESLIGADO" : "normal");
            break;
        case 3:
            s_dbg ^= 8;
            fprintf(stderr, "[VK] F8: combiners %s\n", (s_dbg & 8) ? "DESLIGADOS" : "normais");
            break;
        case 4:
            /* So a imagem; o rastreio de draws existe so no back end OpenGL.
             * No modo direto o frame nao esta na CPU: le o proximo. */
            if (s_async)
                s_f9_pending = 1;
            else
                f9_save();
            break;
        case 7:
            dw4_frame_ring_dump();      /* F12: os ultimos ~2 s de quadros */
            break;
        case 5:
            s_skip_depth_only ^= 1;
            fprintf(stderr, "[VK] F10: draws so de profundidade %s\n",
                    s_skip_depth_only ? "IGNORADOS" : "normais");
            break;
        default:
            s_no_poly_offset ^= 1;
            fprintf(stderr, "[VK] F11: z-bias (depth bias) %s\n",
                    s_no_poly_offset ? "IGNORADO" : "normal");
            break;
        }
    }
}

static void be_flip(void)
{
    static unsigned long tag_draw, tag_tris, tag_flush;
    char tag[320];

    if (s_ok && !s_lost) {
        flush_pending();
        frame_submit_ex(s_async);       /* o que sobrou depois do present_sync */
    }
    /* Etiqueta do quadro na caixa-preta: o que ele teve de diferente. */
    seq_flush_run();
    snprintf(tag, sizeof tag, "sup=%d draws=%lu lotes=%lu tris=%lu tex=%d | %s",
             s_shown ? (int)(s_shown - s_surf) : -1,
             s_n_draw - tag_draw, s_n_flush - tag_flush, s_n_tris - tag_tris, s_tex_count,
             s_seq);
    dw4_frame_tag(tag);
    s_seq_len = 0; s_seq[0] = '\0'; s_seq_surf = -1; s_seq_tex = -1;
    dw4_frame_note(s_last_va, s_last_pitch);
    tag_draw = s_n_draw; tag_tris = s_n_tris; tag_flush = s_n_flush;
    s_shown_stale = 1;
    s_frame++;
    hotkeys_poll();
    if (s_stats) {
        static time_t last;
        static uint32_t frame0;
        time_t now = time(NULL);

        if (!last) { last = now; frame0 = s_frame; }
        if (now - last >= 5) {
            uint32_t nf = s_frame - frame0 ? s_frame - frame0 : 1;
            fprintf(stderr, "[VK] %.1f fps | por frame: %lu draws -> %lu lotes, %lu tris, "
                            "%.1f envios | %lu decodes em %lds | %d texturas, %d superficies, "
                            "%d pipelines | ms/frame: back end %.1f (GPU %.1f, copia p/ janela "
                            "%.1f) | %s, %lu lidos de volta%s\n",
                    (double)(s_frame - frame0) / (double)(now - last),
                    s_n_draw / nf, s_n_flush / nf, s_n_tris / nf,
                    (double)s_n_submit / nf, s_n_decode, (long)(now - last),
                    s_tex_count, s_nsurf, s_npipes,
                    s_t_backend * 1e3 / nf, s_t_gpu * 1e3 / nf, s_t_copy * 1e3 / nf,
                    !s_async ? "modo leitura" : s_swap.ok ? "direto (swapchain)"
                                                          : "direto sem janela",
                    s_n_readback,
                    s_n_odd_present ? " | FRAMES DE SUPERFICIE NAO-TELA" : "");
            {
                /* A thread do jogo no portao do kick (patch 4.13): quantos
                 * kicks por frame, e quanto ela dormiu esperando a cadencia
                 * (flip retido / limitador) e esperando o executor terminar o
                 * que ja tinha sido enviado. O segundo numero e o quanto o
                 * jogo fica preso atras do executor. */
                extern void xbox_Nv2aKickStats(unsigned long *, double *, double *);
                static unsigned long k0;
                static double p0, b0;
                unsigned long k;
                double pw, bw;

                xbox_Nv2aKickStats(&k, &pw, &bw);
                fprintf(stderr, "[VK] jogo: %.1f kicks/frame | ms/frame dormindo: %.1f na "
                        "cadencia, %.1f esperando o executor\n",
                        (double)(k - k0) / nf, (pw - p0) * 1e3 / nf, (bw - b0) * 1e3 / nf);
                k0 = k; p0 = pw; b0 = bw;
            }
            {
                /* metodos do pushbuffer cortados por um kick e retomados (patch 4.12) */
                extern unsigned long nv2a_pb_scan_segment_splits(void);
                static unsigned long last_splits;
                unsigned long sp = nv2a_pb_scan_segment_splits();

                if (sp != last_splits)
                    fprintf(stderr, "[VK] pushbuffer: %lu metodos cortados por um kick e "
                            "retomados em 5 s (total %lu)\n", sp - last_splits, sp);
                last_splits = sp;
            }
            last = now; frame0 = s_frame;
            s_n_draw = s_n_tris = s_n_decode = s_n_flush = s_n_submit = 0;
            tag_draw = tag_tris = tag_flush = 0;
            s_t_backend = s_t_gpu = s_t_copy = 0.0;
            s_n_odd_present = 0;
            s_n_readback = 0;
        }
    }
}

/* ── Inicializacao / encerramento ──────────────────────────────────────────── */

static void be_shutdown(void)
{
    dw4_frame_note_host(NULL, 0, 0, 0);     /* o buffer de leitura vai embora */
    dw4_frame_on_demand(0);
    if (s_dev) {
        vkDeviceWaitIdle(s_dev);
        swap_destroy(0);
        if (s_nsets) {
            set_save();
            for (int i = 0; i < s_nsets; i++) {
                FrameSet *f = &s_sets[i];

                buf_destroy(&f->vb); buf_destroy(&f->vbe);
                buf_destroy(&f->ubo); buf_destroy(&f->stage);
                if (f->dpool) vkDestroyDescriptorPool(s_dev, f->dpool, NULL);
                if (f->fence) vkDestroyFence(s_dev, f->fence, NULL);
                if (f->acquired) vkDestroySemaphore(s_dev, f->acquired, NULL);
                if (f->rendered) vkDestroySemaphore(s_dev, f->rendered, NULL);
            }
        }
        for (int i = 0; i < s_nfb; i++)
            vkDestroyFramebuffer(s_dev, s_fb[i].fb, NULL);
        for (int i = 0; i < s_npipes; i++)
            vkDestroyPipeline(s_dev, s_pipes[i].pipe, NULL);
        for (int i = 0; i < TEX_SLOTS; i++)
            if (s_tex[i].used) img_destroy(&s_tex[i].img);
        for (int i = 0; i < s_nsurf; i++)
            img_destroy(&s_surf[i].color);
        for (int i = 0; i < s_nzeta; i++)
            img_destroy(&s_zeta[i].img);
        img_destroy(&s_white);
        for (int i = 0; i < 18; i++) {
            VkSampler *sp = &s_samp[i / 6][(i / 2) % 3][i % 2];
            if (*sp) vkDestroySampler(s_dev, *sp, NULL);
        }
        buf_destroy(&s_dummy); buf_destroy(&s_rb);
        if (s_vs) vkDestroyShaderModule(s_dev, s_vs, NULL);
        if (s_fs) vkDestroyShaderModule(s_dev, s_fs, NULL);
        if (s_layout) vkDestroyPipelineLayout(s_dev, s_layout, NULL);
        if (s_dsl) vkDestroyDescriptorSetLayout(s_dev, s_dsl, NULL);
        for (int i = 0; i < 2; i++)
            if (s_rp[i]) vkDestroyRenderPass(s_dev, s_rp[i], NULL);
        if (s_pool) vkDestroyCommandPool(s_dev, s_pool, NULL);
        vkDestroyDevice(s_dev, NULL);
    }
    s_dev = VK_NULL_HANDLE;
    swap_destroy(1);                        /* a superficie e da instancia */
    if (s_inst)
        vkDestroyInstance(s_inst, NULL);
    memset(s_sets, 0, sizeof s_sets);
    memset(&s_swap, 0, sizeof s_swap);
    s_swap.acquired = SWAP_NONE;
    s_nsets = 0; s_set = 0; s_async = 0; s_f9_pending = 0;
    memset(&s_vb, 0, sizeof s_vb); memset(&s_vbe, 0, sizeof s_vbe);
    memset(&s_ubo, 0, sizeof s_ubo); memset(&s_stage, 0, sizeof s_stage);
    s_dev = VK_NULL_HANDLE; s_inst = VK_NULL_HANDLE; s_phys = VK_NULL_HANDLE;
    s_pool = VK_NULL_HANDLE; s_fence = VK_NULL_HANDLE; s_dpool = VK_NULL_HANDLE;
    s_layout = VK_NULL_HANDLE; s_dsl = VK_NULL_HANDLE; s_vs = s_fs = VK_NULL_HANDLE;
    s_rp[0] = s_rp[1] = VK_NULL_HANDLE;
    memset(s_samp, 0, sizeof s_samp);
    memset(s_tex, 0, sizeof s_tex);
    memset(s_surf, 0, sizeof s_surf);
    memset(s_zeta, 0, sizeof s_zeta);
    s_nfb = s_npipes = s_nsurf = s_nzeta = s_tex_count = 0;
    s_shown = NULL; s_shown_stale = 1;
    s_recording = 0; s_key_valid = 0; s_pcount = 0;
    s_ok = 0; s_lost = 0;
}

static int pick_device(void)
{
    static const char *type_name[] = { "outro", "integrada", "dedicada", "virtual", "CPU" };
    static const int rank[] = { 1, 3, 4, 2, 0 };   /* por VkPhysicalDeviceType */
    const char *want = getenv("DW4_VK_DEVICE");
    int allow_sw = getenv("DW4_VK_ALLOW_SW") != NULL || getenv("DW4_GL_ALLOW_SW") != NULL;
    VkPhysicalDevice devs[16];
    uint32_t n = 16;
    int best = -1, best_rank = -1;

    if (vkEnumeratePhysicalDevices(s_inst, &n, devs) < 0 || !n) {
        fprintf(stderr, "[VK] nenhuma placa com Vulkan\n");
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties qf[32];
        uint32_t nq = 32;
        int has_gfx = 0, presents = 0, r;

        vkGetPhysicalDeviceProperties(devs[i], &p);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 can = VK_FALSE;

            if (!(qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                continue;
            has_gfx = 1;
            if (s_swap.surf
                    && vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, s_swap.surf, &can)
                       == VK_SUCCESS && can)
                presents = 1;
        }
        r = p.deviceType <= 4 ? rank[p.deviceType] : 0;
        /* Com janela: uma placa que apresenta nela vale mais que uma mais
         * forte que nao apresenta (notebook com a tela na integrada). */
        if (presents)
            r += 10;
        fprintf(stderr, "[VK] placa %u: %s (%s%s)\n", i, p.deviceName,
                p.deviceType <= 4 ? type_name[p.deviceType] : "?",
                s_swap.surf && !presents ? ", nao apresenta nesta janela" : "");
        if (!has_gfx)
            continue;
        if (want && *want) {
            if ((uint32_t)atoi(want) == i) { best = (int)i; best_rank = 99; }
            continue;
        }
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && !allow_sw)
            continue;                   /* lavapipe: so com DW4_VK_ALLOW_SW=1 */
        if (r > best_rank) { best = (int)i; best_rank = r; }
    }
    if (best < 0) {
        fprintf(stderr, "[VK] nenhuma placa acelerada utilizavel\n");
        return 0;
    }
    s_phys = devs[best];
    return 1;
}

static int make_render_pass(int with_depth, VkRenderPass *out)
{
    VkAttachmentDescription att[2] = {
        { .format = COLOR_FMT, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_GENERAL, .finalLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .format = s_depth_fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE,
          .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
          .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL } };
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_GENERAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &cref,
        .pDepthStencilAttachment = with_depth ? &dref : NULL };
    /* O que veio antes deste pass no mesmo command buffer (outro pass que
     * desenhou ou amostrou a imagem, um clear ou a copia de leitura). */
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_TRANSFER_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                      | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                      | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                       | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                       | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = with_depth ? 2 : 1, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };

    return vkCreateRenderPass(s_dev, &ci, NULL, out) == VK_SUCCESS;
}

static int be_init(void)
{
    static const uint32_t white = 0xFFFFFFFFu;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "dw4_recomp", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app };
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceFeatures feat, want = { 0 };
    VkQueueFamilyProperties qf[32];
    uint32_t nq = 32;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .pEnabledFeatures = &want };
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkDescriptorSetLayoutBinding lb[5];
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 5, .pBindings = lb };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &s_dsl };
    VkDescriptorPoolSize psz[2] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, MAX_SETS },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SETS * 4 } };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = MAX_SETS, .poolSizeCount = 2, .pPoolSizes = psz };
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof VS_SPV, .pCode = VS_SPV };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof FS_SPV, .pCode = FS_SPV };
    VkFormatProperties fp;
    const char *c, *pm;
    const char *inst_ext[16];
    unsigned n_inst_ext = 16;
    static const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    int want_direct;
    VkResult r;

    if (s_ok)
        return 1;
    c = getenv("DW4_VK_CULL");
    if (c)
        s_cull_mode = strcmp(c, "inv") == 0 ? 2 : (c[0] == '0' ? 0 : 1);
    s_stats = getenv("DW4_VK_STATS") != NULL || getenv("DW4_GL_STATS") != NULL;
    if (getenv("DW4_VK_DBG"))
        s_dbg = atoi(getenv("DW4_VK_DBG"));
    ro_init("VK");

    /* Modo de apresentacao: direto (swapchain) se o toolkit abriu a janela
     * para isso; senao leitura de volta. */
    pm = getenv("DW4_VK_PRESENT");
    want_direct = !(pm && !strcmp(pm, "readback")) && xbox_FramebufferWindowVulkan();
    if (want_direct && xbox_FramebufferWindowVkExtensions(inst_ext, &n_inst_ext)) {
        ici.enabledExtensionCount = n_inst_ext;
        ici.ppEnabledExtensionNames = inst_ext;
    } else {
        want_direct = 0;
    }
    r = vkCreateInstance(&ici, NULL, &s_inst);
    if (r != VK_SUCCESS && want_direct) {
        fprintf(stderr, "[VK] instancia com as extensoes de janela falhou (%d); "
                        "sem apresentacao direta\n", (int)r);
        want_direct = 0;
        ici.enabledExtensionCount = 0;
        r = vkCreateInstance(&ici, NULL, &s_inst);
    }
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateInstance falhou (%d): sem driver Vulkan?\n", (int)r);
        s_inst = VK_NULL_HANDLE;
        return 0;
    }
    if (want_direct && !xbox_FramebufferWindowVkSurface(s_inst, &s_swap.surf)) {
        s_swap.surf = VK_NULL_HANDLE;
        want_direct = 0;
    }
    if (!pick_device())
        goto fail;
    vkGetPhysicalDeviceProperties(s_phys, &props);
    vkGetPhysicalDeviceFeatures(s_phys, &feat);
    vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memprops);
    vkGetPhysicalDeviceQueueFamilyProperties(s_phys, &nq, qf);
    for (s_qfam = 0; s_qfam < nq; s_qfam++)
        if (qf[s_qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (s_swap.surf) {
        /* A fila que desenha tambem apresenta (toda placa de mesa faz isso
         * na mesma familia); senao, sem apresentacao direta. */
        uint32_t q;

        for (q = 0; q < nq; q++) {
            VkBool32 can = VK_FALSE;

            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                    && vkGetPhysicalDeviceSurfaceSupportKHR(s_phys, q, s_swap.surf, &can)
                       == VK_SUCCESS && can)
                break;
        }
        if (q < nq) {
            VkExtensionProperties *ep;
            uint32_t ne = 0;

            s_qfam = q;
            vkEnumerateDeviceExtensionProperties(s_phys, NULL, &ne, NULL);
            ep = ne ? malloc(ne * sizeof *ep) : NULL;
            if (ep && vkEnumerateDeviceExtensionProperties(s_phys, NULL, &ne, ep) >= 0)
                for (uint32_t i = 0; i < ne; i++)
                    if (!strcmp(ep[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                        s_swap.have_ext = 1;
            free(ep);
        }
        if (!s_swap.have_ext)
            fprintf(stderr, "[VK] esta placa nao apresenta na janela; usando leitura de volta\n");
    }
    if (s_swap.have_ext) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_ext;
    }
    s_depth_clamp = feat.depthClamp == VK_TRUE;
    want.depthClamp = feat.depthClamp;

    /* D24S8 e o formato do Xbox; onde nao existe (AMD), D32F + S8. */
    vkGetPhysicalDeviceFormatProperties(s_phys, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    s_depth_fmt = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;

    qci.queueFamilyIndex = s_qfam;
    r = vkCreateDevice(s_phys, &dci, NULL, &s_dev);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[VK] vkCreateDevice falhou (%d)\n", (int)r);
        s_dev = VK_NULL_HANDLE;
        goto fail;
    }
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

    pci.queueFamilyIndex = s_qfam;
    if (vkCreateCommandPool(s_dev, &pci, NULL, &s_pool) != VK_SUCCESS)
        goto fail;
    if (!make_render_pass(0, &s_rp[0]) || !make_render_pass(1, &s_rp[1]))
        goto fail;

    memset(lb, 0, sizeof lb);
    lb[0].binding = 0;
    lb[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    lb[0].descriptorCount = 1;
    lb[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < 5; i++) {
        lb[i].binding = i;
        lb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if (vkCreateDescriptorSetLayout(s_dev, &lci, NULL, &s_dsl) != VK_SUCCESS
            || vkCreatePipelineLayout(s_dev, &plci, NULL, &s_layout) != VK_SUCCESS
            || vkCreateShaderModule(s_dev, &smv, NULL, &s_vs) != VK_SUCCESS
            || vkCreateShaderModule(s_dev, &smf, NULL, &s_fs) != VK_SUCCESS)
        goto fail;

    s_ubo_step = (uint32_t)sizeof(Ubo);
    if (props.limits.minUniformBufferOffsetAlignment > 1) {
        uint32_t a = (uint32_t)props.limits.minUniformBufferOffsetAlignment;
        s_ubo_step = (s_ubo_step + a - 1) / a * a;
    }
    /* Apresentacao direta: swapchain na janela + dois conjuntos de frame.
     * DW4_VK_PRESENT=async liga os quadros em voo mesmo sem janela (teste). */
    if (s_swap.surf && s_swap.have_ext && !swap_create()) {
        fprintf(stderr, "[VK] swapchain nao subiu; usando leitura de volta\n");
        swap_destroy(1);
    }
    if (s_swap.surf && !s_swap.ok)
        swap_destroy(1);
    s_async = s_swap.ok || (pm && !strcmp(pm, "async"));
    for (s_nsets = 0; s_nsets < (s_async ? FRAME_SETS : 1); s_nsets++) {
        FrameSet *f = &s_sets[s_nsets];
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkCommandBuffer cbs[2];

        cai.commandPool = s_pool;
        if (vkAllocateCommandBuffers(s_dev, &cai, cbs) != VK_SUCCESS
                || vkCreateFence(s_dev, &fci, NULL, &f->fence) != VK_SUCCESS
                || vkCreateDescriptorPool(s_dev, &dpci, NULL, &f->dpool) != VK_SUCCESS
                || vkCreateSemaphore(s_dev, &sci, NULL, &f->acquired) != VK_SUCCESS
                || vkCreateSemaphore(s_dev, &sci, NULL, &f->rendered) != VK_SUCCESS
                || !buf_create(&f->vb, (VkDeviceSize)VTX_CAP * sizeof(Nv2aVertex),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
                || !buf_create(&f->vbe, (VkDeviceSize)VTX_CAP * sizeof(Nv2aVertexExt),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
                || !buf_create(&f->ubo, UBO_CAP, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
                || !buf_create(&f->stage, 16u * 1024u * 1024u,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            s_nsets++;                  /* o shutdown libera o que este criou */
            goto fail;
        }
        f->up = cbs[0];
        f->draw = cbs[1];
    }
    set_load(0);
    if (!buf_create(&s_dummy, sizeof(Nv2aVertexExt), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
            || (s_buf_cached = 1,
                !buf_create(&s_rb, 640u * 480u * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT)))
        goto fail;
    memset(s_dummy.map, 0, sizeof(Nv2aVertexExt));

    if (!img_create(&s_white, 1, 1, COLOR_FMT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT))
        goto fail;
    frame_begin();
    tex_upload(&s_white, &white, 1, 1, 1);
    frame_submit();
    if (s_lost)
        goto fail;

    fprintf(stderr, "[VK] back end Vulkan em %s (Vulkan %u.%u.%u, depth %s%s)\n",
            props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
            VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion),
            s_depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32FS8",
            s_depth_clamp ? "" : ", sem depth clamp");
    fprintf(stderr, "[VK] apresentacao: %s\n",
            s_swap.ok ? "direta pela GPU (swapchain), quadros em voo"
            : s_async ? "quadros em voo, sem janela (DW4_VK_PRESENT=async)"
                      : "leitura de volta (sem janela Vulkan)");
    dw4_frame_on_demand(s_async);
    s_ok = 1;
    return 1;

fail:
    fprintf(stderr, "[VK] inicializacao do Vulkan falhou\n");
    be_shutdown();
    return 0;
}

/* O que o host (gl_host.c) chama. Mude DW4_GL_MODULE_ABI se a struct mudar. */
static void tm_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    double t0 = now_s();
    be_clear(s, rs, flags, argb, zstencil);
    s_t_backend += now_s() - t0;
}

static void tm_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    double t0 = now_s();
    be_draw(s, b);
    s_t_backend += now_s() - t0;
}

static void tm_present_sync(void)
{
    double t0 = now_s();
    be_present_sync();
    s_t_backend += now_s() - t0;
}

static void tm_flip(void)
{
    double t0 = now_s();
    be_flip();
    s_t_backend += now_s() - t0;
}

static const Dw4GlModule s_module = {
    .abi = DW4_GL_MODULE_ABI, .name = "Vulkan", .needs_gl = 0,
    .init = be_init, .shutdown = be_shutdown,
    .clear = tm_clear, .draw = tm_draw, .flip = tm_flip, .present_sync = tm_present_sync
};

__attribute__((visibility("default")))
const Dw4GlModule *dw4_gl_module(void)
{
    return &s_module;
}
