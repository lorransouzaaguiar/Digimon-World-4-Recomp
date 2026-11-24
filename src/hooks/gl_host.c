/**
 * Host dos renderizadores: escolhe a API grafica, carrega o modulo dela e o
 * RECARREGA A QUENTE.
 *
 *     DW4_RENDERER=opengl   libdw4_gl.so (nv2a_gl_backend.c) -- padrao
 *     DW4_RENDERER=vulkan   libdw4_vk.so (nv2a_vk_backend.c)
 *     DW4_RENDERER=cpu      rasterizador em CPU do toolkit (= DW4_SW_RENDER=1)
 *
 * E o que o launcher grava quando o jogador escolhe a API em "Graficos". Se a
 * API pedida nao sobe (sem driver, sem a biblioteca), tenta a outra e, por
 * ultimo, a CPU -- o jogo abre de qualquer forma e o log diz o que aconteceu.
 *
 * Para o OpenGL o host e dono do contexto EGL; o modulo Vulkan cria o proprio
 * dispositivo (init/shutdown em gl_module.h).
 *
 * Para que: corrigir o renderizador com o jogo aberto. Quem esta jogando fica
 * parado onde o defeito aparece; recompila-se (cmake --build build) e, quando
 * build/libdw4_gl.so muda no disco, o jogo troca de renderizador sozinho no
 * proximo flip -- sem reiniciar, sem perder o ponto do jogo. Nao e um save
 * state: so o codigo de desenho e trocado; o estado do jogo nem e tocado.
 *
 * Como:
 *   - o host e dono do contexto EGL offscreen (plataforma "device", 3.3 core,
 *     recusa renderizador por software salvo DW4_GL_ALLOW_SW=1);
 *   - registra no executor um Nv2aBackend fixo cujas funcoes repassam para a
 *     tabela do modulo carregado;
 *   - a cada ~0,5 s (no flip, na thread NV2A) confere o mtime da biblioteca.
 *     Mudou e ficou estavel por 0,4 s -> copia para build/gl_live/ com nome
 *     unico (dlopen do mesmo caminho devolveria a copia antiga), carrega,
 *     e so entao destroi o contexto antigo (levando todos os objetos GL do
 *     modulo velho) e cria um novo para o modulo novo;
 *   - se a biblioteca nova nao carregar (erro de link, ABI diferente), o
 *     modulo antigo continua valendo e o motivo vai para o log.
 *
 *   DW4_SW_RENDER=1     nao usa nada disto: rasterizador em CPU do toolkit
 *   DW4_GL_ALLOW_SW=1   aceita OpenGL por software (llvmpipe)
 *   DW4_GL_NO_RELOAD=1  carrega o modulo uma vez e nunca recarrega
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "dw4_hooks.h"
#include "gl_module.h"

/* ── Contexto EGL ──────────────────────────────────────────────────────────── */

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLConfig  s_cfg;
static EGLContext s_ctx = EGL_NO_CONTEXT;
static EGLSurface s_pbuf = EGL_NO_SURFACE;
static int s_current;                 /* contexto corrente na thread NV2A */
static int s_allow_sw;

static const EGLint CTX_ATTR[] = {
    EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_NONE
};

static int try_display(EGLDisplay dpy, const char *what)
{
    static const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
    };
    static const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLint major, minor, n = 0;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface pb;
    const char *renderer;

    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
        fprintf(stderr, "[GL] %s: eglInitialize falhou (0x%X)\n", what,
                (unsigned)eglGetError());
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)
            || !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "[GL] %s: sem config OpenGL/pbuffer (0x%X, n=%d)\n", what,
                (unsigned)eglGetError(), (int)n);
        goto fail;
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, CTX_ATTR);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[GL] %s: eglCreateContext 3.3 core falhou (0x%X)\n", what,
                (unsigned)eglGetError());
        goto fail;
    }
    pb = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (pb == EGL_NO_SURFACE || !eglMakeCurrent(dpy, pb, pb, ctx)) {
        fprintf(stderr, "[GL] %s: pbuffer/MakeCurrent falhou (0x%X)\n", what,
                (unsigned)eglGetError());
        if (pb != EGL_NO_SURFACE)
            eglDestroySurface(dpy, pb);
        eglDestroyContext(dpy, ctx);
        goto fail;
    }
    renderer = (const char *)glGetString(GL_RENDERER);
    if (!s_allow_sw && (!renderer || strstr(renderer, "llvmpipe")
            || strstr(renderer, "softpipe") || strstr(renderer, "SwiftShader"))) {
        fprintf(stderr, "[GL] %s: renderizador por software (%s), ignorado\n",
                what, renderer ? renderer : "?");
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(dpy, pb);
        eglDestroyContext(dpy, ctx);
        goto fail;
    }
    fprintf(stderr, "[GL] back end OpenGL em %s: %s (%s)\n", what, renderer,
            (const char *)glGetString(GL_VERSION));
    s_dpy = dpy; s_cfg = cfg; s_ctx = ctx; s_pbuf = pb;
    /* solta: o contexto passa para a thread NV2A na primeira chamada */
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return 1;
fail:
    eglTerminate(dpy);
    return 0;
}

static int context_create(void)
{
    const char *ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    if (ext && strstr(ext, "EGL_EXT_device_enumeration")
            && strstr(ext, "EGL_EXT_platform_device")) {
        EGLDeviceEXT devs[16];
        EGLint n = 0;

        if (eglQueryDevicesEXT(16, devs, &n)) {
            for (EGLint i = 0; i < n; i++) {
                char what[96];
                const char *dx = strstr(ext, "EGL_EXT_device_query")
                    ? eglQueryDeviceStringEXT(devs[i], EGL_EXTENSIONS) : NULL;
                snprintf(what, sizeof what, "EGL device %d%s", (int)i,
                         dx && strstr(dx, "NV_device_cuda") ? " (NVIDIA)"
                       : dx && strstr(dx, "MESA_device_software") ? " (software)" : "");
                if (try_display(eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT,
                                                         devs[i], NULL), what))
                    return 1;
            }
        }
    }
    return try_display(eglGetDisplay(EGL_DEFAULT_DISPLAY), "EGL default");
}

static const Dw4GlModule *s_mod;

static int ensure_current(void)
{
    if (s_current || !s_mod->needs_gl)  /* Vulkan: nao ha contexto a tornar corrente */
        return 1;
    if (!eglMakeCurrent(s_dpy, s_pbuf, s_pbuf, s_ctx)) {
        LOG_ONCE("[GL] eglMakeCurrent falhou na thread NV2A (0x%X)\n",
                 (unsigned)eglGetError());
        return 0;
    }
    s_current = 1;
    return 1;
}

/* Contexto novo no mesmo display: todos os objetos GL do modulo antigo vao
 * embora com o contexto antigo. Chamado na thread NV2A. */
static int context_recreate(void)
{
    EGLContext fresh;

    /* A API corrente do EGL e por thread e o padrao e OpenGL ES: sem isto a
     * thread NV2A pedia um contexto ES com atributos de perfil core e levava
     * EGL_BAD_ATTRIBUTE. */
    eglBindAPI(EGL_OPENGL_API);
    fresh = eglCreateContext(s_dpy, s_cfg, EGL_NO_CONTEXT, CTX_ATTR);

    if (fresh == EGL_NO_CONTEXT) {
        fprintf(stderr, "[GL] reload: eglCreateContext falhou (0x%X)\n",
                (unsigned)eglGetError());
        return 0;
    }
    eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(s_dpy, s_ctx);
    s_ctx = fresh;
    s_current = 0;
    return ensure_current();
}

/* ── Modulo ────────────────────────────────────────────────────────────────── */

static char s_exe_dir[PATH_MAX];
static char s_lib_path[PATH_MAX];     /* <dir do executavel>/libdw4_{gl,vk}.so */
static char s_live_dir[PATH_MAX];     /* <dir do executavel>/gl_live */
static struct timespec s_loaded_mtime;
static int s_no_reload;
static unsigned s_generation;

static int copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out;
    char buf[65536];
    size_t n;
    int ok = 1;

    if (!in)
        return 0;
    out = fopen(to, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n)
            ok = 0;
    fclose(in);
    if (fclose(out) != 0)
        ok = 0;
    return ok;
}

/* Carrega a biblioteca atual do disco. Nao troca nada: devolve a tabela. */
static const Dw4GlModule *module_open(struct timespec *mtime)
{
    const Dw4GlModule *(*entry)(void);
    const Dw4GlModule *m;
    struct stat st;
    char live[PATH_MAX + 64];
    void *h;

    if (stat(s_lib_path, &st) != 0) {
        fprintf(stderr, "[GL] modulo nao encontrado: %s\n", s_lib_path);
        return NULL;
    }
    if (s_no_reload) {
        /* Sem recarga (pacotes de release): carrega do proprio lugar. A pasta
         * do programa pode ser so de leitura (AppImage), entao nada de copia. */
        h = dlopen(s_lib_path, RTLD_NOW | RTLD_LOCAL);
    } else {
        mkdir(s_live_dir, 0755);
        snprintf(live, sizeof live, "%s/libdw4_live.%d.%u.so", s_live_dir, (int)getpid(),
                 s_generation);
        if (!copy_file(s_lib_path, live)) {
            fprintf(stderr, "[GL] nao copiou o modulo para %s\n", live);
            return NULL;
        }
        h = dlopen(live, RTLD_NOW | RTLD_LOCAL);
        unlink(live);                   /* o mapeamento continua valido */
    }
    if (!h) {
        fprintf(stderr, "[GL] dlopen falhou: %s\n", dlerror());
        return NULL;
    }
    entry = (const Dw4GlModule *(*)(void))dlsym(h, "dw4_gl_module");
    m = entry ? entry() : NULL;
    if (!m || m->abi != DW4_GL_MODULE_ABI) {
        fprintf(stderr, "[GL] modulo incompativel (abi %d, esperado %d); "
                        "reinicie o jogo\n", m ? m->abi : -1, DW4_GL_MODULE_ABI);
        dlclose(h);
        return NULL;
    }
    *mtime = st.st_mtim;
    s_generation++;
    return m;                           /* a biblioteca antiga fica mapeada: */
}                                       /* poucos MB por reload, sem risco   */

static void reload_check(void)
{
    static unsigned tick;
    static struct timespec pending;     /* mtime visto, esperando estabilizar */
    static double pending_since;
    const Dw4GlModule *fresh;
    struct timespec mt, now;
    struct stat st;
    double t;

    if (s_no_reload || (++tick % 30) != 0 || stat(s_lib_path, &st) != 0)
        return;
    if (st.st_mtim.tv_sec == s_loaded_mtime.tv_sec
            && st.st_mtim.tv_nsec == s_loaded_mtime.tv_nsec)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    t = (double)now.tv_sec + (double)now.tv_nsec / 1e9;
    if (st.st_mtim.tv_sec != pending.tv_sec || st.st_mtim.tv_nsec != pending.tv_nsec) {
        pending = st.st_mtim;           /* o linker ainda pode estar escrevendo */
        pending_since = t;
        return;
    }
    if (t - pending_since < 0.4 || st.st_size == 0)
        return;

    fresh = module_open(&mt);
    if (!fresh) {
        s_loaded_mtime = st.st_mtim;    /* nao tenta de novo a mesma versao ruim */
        fprintf(stderr, "[GL] reload CANCELADO: o renderizador antigo continua\n");
        return;
    }
    if (fresh->needs_gl != s_mod->needs_gl) {
        s_loaded_mtime = st.st_mtim;
        fprintf(stderr, "[GL] reload CANCELADO: a biblioteca mudou de API grafica\n");
        return;
    }
    if (fresh->needs_gl) {
        if (!context_recreate()) {
            s_loaded_mtime = st.st_mtim;    /* uma tentativa por versao do arquivo */
            fprintf(stderr, "[GL] reload CANCELADO: sem contexto novo; o renderizador "
                            "antigo continua\n");
            return;
        }
    } else {
        /* Dono do proprio dispositivo: o antigo solta tudo, o novo cria. */
        if (s_mod->shutdown)
            s_mod->shutdown();
        if (fresh->init && !fresh->init()) {
            s_loaded_mtime = st.st_mtim;
            if (s_mod->init && s_mod->init()) {
                fprintf(stderr, "[GL] reload CANCELADO: o modulo novo nao iniciou; o "
                                "renderizador antigo voltou\n");
            } else {
                fprintf(stderr, "[GL] reload FALHOU e o renderizador antigo nao voltou: "
                                "reinicie o jogo\n");
                s_mod = NULL;
            }
            return;
        }
    }
    s_mod = fresh;
    s_loaded_mtime = mt;
    fprintf(stderr, "[GL] renderizador %s RECARREGADO (versao %u)\n", s_mod->name,
            s_generation);
}

/* ── Back end fixo registrado no executor ──────────────────────────────────── */

static void host_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                       uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    if (s_mod && ensure_current())
        s_mod->clear(s, rs, flags, argb, zstencil);
}

static void host_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    if (s_mod && ensure_current())
        s_mod->draw(s, b);
}

static void host_present_sync(void)
{
    if (s_mod && (s_current || !s_mod->needs_gl))
        s_mod->present_sync();
}

static void host_flip(void)
{
    if (s_mod && ensure_current())
        s_mod->flip();
    else
        dw4_frame_note(0, 0);
    reload_check();                     /* entre dois frames: nada pendente */
}

static const Nv2aBackend s_backend = { host_clear, host_draw, host_flip };

/* Rasterizador em CPU: um back end so com flip nao desvia o desenho (o
 * executor so entrega clear/draw se os ponteiros existirem), mas mantem a
 * contagem de frames para o amostrador de travamento e o roteiro. */
static void sw_flip(void) { dw4_frame_note(0, 0); }
static const Nv2aBackend s_sw_backend = { NULL, NULL, sw_flip };

/* Carrega e inicia um renderizador. 0 se ele nao sobe nesta maquina. */
static int renderer_start(int vulkan)
{
    snprintf(s_lib_path, sizeof s_lib_path, "%s/libdw4_%s.so", s_exe_dir,
             vulkan ? "vk" : "gl");
    s_mod = module_open(&s_loaded_mtime);
    if (!s_mod)
        return 0;
    if (s_mod->needs_gl && !context_create()) {
        fprintf(stderr, "[GL] nenhum contexto OpenGL 3.3 acelerado\n");
        s_mod = NULL;
        return 0;
    }
    if (s_mod->init && !s_mod->init()) {
        s_mod = NULL;
        return 0;
    }
    return 1;
}

void dw4_gl_backend_init(void)
{
    const char *r = getenv("DW4_SW_RENDER"), *api = getenv("DW4_RENDERER");
    char exe[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    char *slash;
    int vulkan = 0;

    if ((r && *r && *r != '0') || (api && (!strcmp(api, "cpu") || !strcmp(api, "sw")))) {
        fprintf(stderr, "[GL] rasterizador em CPU do toolkit (DW4_SW_RENDER / "
                        "DW4_RENDERER=cpu)\n");
        nv2a_backend_register(&s_sw_backend);
        return;
    }
    if (api && *api) {
        if (!strcmp(api, "vulkan") || !strcmp(api, "vk"))
            vulkan = 1;
        else if (strcmp(api, "opengl") && strcmp(api, "gl"))
            fprintf(stderr, "[GL] DW4_RENDERER=%s desconhecido (opengl, vulkan, cpu); "
                            "usando opengl\n", api);
    }
    s_allow_sw = getenv("DW4_GL_ALLOW_SW") != NULL;
    s_no_reload = getenv("DW4_GL_NO_RELOAD") != NULL;

    if (len <= 0)
        goto fallback;
    exe[len] = '\0';
    slash = strrchr(exe, '/');
    if (!slash)
        goto fallback;
    *slash = '\0';
    snprintf(s_exe_dir, sizeof s_exe_dir, "%s", exe);
    snprintf(s_live_dir, sizeof s_live_dir, "%s/gl_live", exe);

    if (!renderer_start(vulkan)) {
        fprintf(stderr, "[GL] %s indisponivel; tentando %s\n",
                vulkan ? "Vulkan" : "OpenGL", vulkan ? "OpenGL" : "Vulkan");
        vulkan = !vulkan;
        if (!renderer_start(vulkan))
            goto fallback;
    }
    /* Linha lida pelo launcher e pelos testes: qual API ficou valendo. */
    fprintf(stderr, "[GL] renderizador ativo: %s (%s)%s\n", s_mod->name, s_lib_path,
            s_no_reload ? "" : " -- recarrega sozinho quando o arquivo mudar");
    nv2a_backend_register_with_present_sync(&s_backend, host_present_sync);
    return;

fallback:
    s_mod = NULL;
    fprintf(stderr, "[GL] usando o rasterizador em CPU\n");
    nv2a_backend_register(&s_sw_backend);
}
