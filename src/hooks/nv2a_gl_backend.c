/**
 * Back end OpenGL 3.3 para o executor de pushbuffer NV2A (nv2a_backend.h).
 *
 * Mesmo desenho dos outros ports Linux do xboxrecomp (Amped 2 / SSX 3 usam um
 * back end Vulkan na mesma interface): o executor continua decodificando o
 * pushbuffer, transformando vertices e recortando no near plane na CPU, mas
 * em vez de rasterizar na CPU entrega os triangulos (em pixels da superficie)
 * para a GPU.
 *
 *   contexto      EGL offscreen, criado e mantido pelo host (gl_host.c); este
 *                 arquivo e o modulo recarregavel libdw4_gl.so
 *   superficies   um FBO por superficie de cor do guest (chave: color_va),
 *                 depth/stencil por zeta_va
 *   texturas      nv2a_backend_decode_texture -> BGRA8, cache por
 *                 (endereco, formato, tamanho); um hash dos bytes do guest e
 *                 conferido uma vez por frame (FMV reescreve no lugar)
 *   render-to-texture  textura cujo endereco e uma superficie da GPU amostra
 *                 a imagem do FBO
 *   apresentacao  no flip a superficie exibida e lida de volta (glReadPixels,
 *                 640x480) para a memoria do guest; a janela SDL do toolkit,
 *                 o teclado/controle e o DW4_SHOT continuam como estavam
 *
 *   combiners     lotes transformados chegam com Nv2aBatchExt (patch 4.6 do
 *                 toolkit): os register combiners, os 4 estagios de textura,
 *                 especular e fog. O fragment shader e um porte linha a linha
 *                 de rc_stage_fetch + nv2a_rc_eval (nv2a_pb_exec.c /
 *                 nv2a_combiner.c). Lotes em espaco de tela continuam
 *                 texel x difusa, como no caminho em CPU.
 *   lotes         draws consecutivos com o mesmo estado sao fundidos num unico
 *                 glDrawArrays (o lobby manda ~4.800 draws por frame)
 *
 *   DW4_SW_RENDER=1    volta ao rasterizador em CPU do toolkit (comparacao)
 *   DW4_GL_CULL=0|inv  desliga / inverte o culling (depuracao)
 *   DW4_GL_STATS=1     contadores a cada 5 s
 *   F5..F9 na janela   diagnostico ao vivo (ver hotkeys_poll)
 *   DW4_GL_ALLOW_SW=1  aceita um OpenGL por software (llvmpipe) -- so para
 *                      validar a imagem numa maquina sem GPU utilizavel
 */

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nv2a_backend.h"
#include "xbox_memory_layout.h"
#include "dw4_hooks.h"
#include "gl_module.h"
#include "render_opts.h"

#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)

/* ── Estado do modulo ──────────────────────────────────────────────────────
 * Este arquivo e compilado como libdw4_gl.so e recarregado a quente pelo host
 * (gl_host.c), que e dono do contexto EGL e o deixa corrente na thread NV2A
 * antes de qualquer chamada. Tudo aqui e estado do modulo: some no reload. */
static int s_inited;                  /* program_init ja rodou neste contexto */
static int s_cull_mode = 1;           /* 0 off, 1 normal, 2 giro cru (sem a troca CW/CCW) */
static int s_stats;

/* ── Programa ──────────────────────────────────────────────────────────────── */

static GLuint s_prog, s_vao, s_vbo, s_vbo_ext, s_white;
static GLint u_surf, u_texsize, u_alpha, u_rc, u_tscale, u_fogc;
static GLint u_cicw, u_aicw, u_cocw, u_aocw, u_f0, u_f1;
static GLint u_final, u_fc0, u_fc1, u_ctl;

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos;\n"     /* x, y, z em pixels; rhw */
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec2 a_uv;\n"      /* texels (caminho simples) */
    "layout(location=3) in vec4 a_t0;\n"      /* s,t,r,q por estagio */
    "layout(location=4) in vec4 a_t1;\n"
    "layout(location=5) in vec4 a_t2;\n"
    "layout(location=6) in vec4 a_t3;\n"
    "layout(location=7) in vec4 a_spec;\n"
    "layout(location=8) in float a_fog;\n"
    "uniform vec3 u_surf;\n"                  /* largura, altura, zmax */
    "uniform vec2 u_texsize;\n"
    "out vec4 v_col; out vec2 v_uv; out vec4 v_t0, v_t1, v_t2, v_t3, v_spec;\n"
    "out float v_fog;\n"
    "void main(){\n"
    "  float w = (a_pos.w > 0.0) ? 1.0 / a_pos.w : 1.0;\n"
    "  vec3 ndc = vec3(a_pos.x / u_surf.x, a_pos.y / u_surf.y,\n"
    "                  a_pos.z / u_surf.z) * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc * w, w);\n"
    "  v_col = a_col; v_uv = a_uv / u_texsize;\n"
    "  v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;\n"
    "  v_spec = a_spec; v_fog = a_fog;\n"
    "}\n";

/* Porte de nv2a_rc_eval (nv2a_combiner.c) e rc_stage_fetch (nv2a_pb_exec.c);
 * os nomes e a ordem das operacoes seguem o C para poderem ser conferidos. */
static const char *FS =
    "#version 330 core\n"
    "in vec4 v_col; in vec2 v_uv; in vec4 v_t0, v_t1, v_t2, v_t3, v_spec;\n"
    "in float v_fog;\n"
    "uniform sampler2D u_tex0, u_tex1, u_tex2, u_tex3;\n"
    "uniform ivec2 u_alpha;\n"                /* funcao (0 = sem teste), ref */
    "uniform int u_rc;\n"                     /* 0: texel x difusa */
    "uniform vec2 u_tscale[4];\n"
    "uniform vec4 u_fogc;\n"
    "uniform uint u_cicw[8], u_aicw[8], u_cocw[8], u_aocw[8];\n"
    "uniform vec4 u_f0[8], u_f1[8], u_fc0, u_fc1;\n"
    "uniform uvec2 u_final;\n"
    "uniform uvec3 u_ctl;\n"                  /* control, stage_program, clip_plane_mode */
    "out vec4 o;\n"
    "vec4 R[16];\n"
    "float map_in(float x, uint b){\n"
    "  float p = max(x, 0.0);\n"
    "  switch ((b >> 5u) & 7u) {\n"
    "    case 0u: return p;              case 1u: return 1.0 - clamp(x, 0.0, 1.0);\n"
    "    case 2u: return 2.0 * p - 1.0;  case 3u: return -2.0 * p + 1.0;\n"
    "    case 4u: return p - 0.5;        case 5u: return -p + 0.5;\n"
    "    case 6u: return x;              default: return -x;\n"
    "  }\n"
    "}\n"
    "float map_out(float x, uint m){\n"
    "  switch (m) {\n"
    "    case 0x08u: return x - 0.5;         case 0x10u: return x * 2.0;\n"
    "    case 0x18u: return (x - 0.5) * 2.0; case 0x20u: return x * 4.0;\n"
    "    case 0x30u: return x * 0.5;         default:    return x;\n"
    "  }\n"
    "}\n"
    "vec3 mo3(vec3 v, uint m){\n"
    "  return clamp(vec3(map_out(v.x, m), map_out(v.y, m), map_out(v.z, m)), -1.0, 1.0);\n"
    "}\n"
    "vec3 rd_rgb(uint b){\n"
    "  vec4 r = R[b & 0xFu];\n"
    "  vec3 v = ((b & 0x10u) != 0u) ? vec3(r.a) : r.rgb;\n"
    "  return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
    "}\n"
    "float rd_a(uint b){\n"
    "  vec4 r = R[b & 0xFu];\n"
    "  return map_in(((b & 0x10u) != 0u) ? r.a : r.b, b);\n"
    "}\n"
    "bool g_kill = false;\n"
    "vec4 stage(int st, vec4 c, sampler2D s){\n"
    "  uint mode = (u_ctl.y >> uint(st * 5)) & 0x1Fu;\n"
    "  float q = (c.w != 0.0) ? c.w : 1.0;\n"
    "  if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);\n"
    "  if (mode == 1u || mode == 2u) return texture(s, c.xy / q * u_tscale[st]);\n"
    "  if (mode == 3u) return texture(s, c.xy * u_tscale[st]);\n"
    "  if (mode == 4u) return clamp(c, 0.0, 1.0);\n"
    "  if (mode == 5u) {\n"
    "    for (int j = 0; j < 4; j++) {\n"
    "      bool ge = ((u_ctl.z >> uint(st * 4 + j)) & 1u) != 0u;\n"
    "      if (ge ? (c[j] >= 0.0) : (c[j] < 0.0)) g_kill = true;\n"
    "    }\n"
    "  }\n"
    "  return vec4(0.0);\n"
    "}\n"
    "vec4 rc_eval(vec4 t0, vec4 t1, vec4 t2, vec4 t3){\n"
    "  uint n = min(u_ctl.x & 0xFFu, 8u), flags = u_ctl.x >> 8u;\n"
    "  for (int i = 0; i < 16; i++) R[i] = vec4(0.0);\n"
    "  R[3] = vec4(u_fogc.rgb, v_fog); R[4] = v_col; R[5] = v_spec;\n"
    "  R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;\n"
    "  R[12].a = ((u_ctl.y & 0x1Fu) != 0u) ? t0.a : 1.0;\n"
    "  for (uint s = 0u; s < n; s++) {\n"
    "    uint icw = u_cicw[s], ocw = u_cocw[s], aicw = u_aicw[s], aocw = u_aocw[s];\n"
    "    uint fl = ocw >> 12u, afl = aocw >> 12u;\n"
    "    R[1] = u_f0[((flags & 0x010u) != 0u) ? s : 0u];\n"
    "    R[2] = u_f1[((flags & 0x100u) != 0u) ? s : 0u];\n"
    "    bool mux_cd = ((flags & 1u) != 0u) ? (R[12].a >= 0.5)\n"
    "                                       : ((int(R[12].a * 255.0) & 1) != 0);\n"
    "    vec3 A = rd_rgb(icw >> 24u), B = rd_rgb(icw >> 16u);\n"
    "    vec3 C = rd_rgb(icw >> 8u),  D = rd_rgb(icw);\n"
    "    float aA = rd_a(aicw >> 24u), aB = rd_a(aicw >> 16u);\n"
    "    float aC = rd_a(aicw >> 8u),  aD = rd_a(aicw);\n"
    "    vec3 pab = ((fl & 2u) != 0u) ? vec3(dot(A, B)) : A * B;\n"
    "    vec3 pcd = ((fl & 1u) != 0u) ? vec3(dot(C, D)) : C * D;\n"
    "    vec3 sum = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;\n"
    "    vec3 ab = mo3(pab, fl & 0x38u), cd = mo3(pcd, fl & 0x38u), ms = mo3(sum, fl & 0x38u);\n"
    "    float apab = aA * aB, apcd = aC * aD;\n"
    "    float asum = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;\n"
    "    float aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);\n"
    "    float acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);\n"
    "    float ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);\n"
    "    uint d;\n"
    "    d = (ocw >> 4u) & 0xFu;\n"
    "    if (d != 0u) { R[d].rgb = ab; if ((fl & 0x80u) != 0u) R[d].a = ab.b; }\n"
    "    d = ocw & 0xFu;\n"
    "    if (d != 0u) { R[d].rgb = cd; if ((fl & 0x40u) != 0u) R[d].a = cd.b; }\n"
    "    d = (ocw >> 8u) & 0xFu;  if (d != 0u) R[d].rgb = ms;\n"
    "    d = (aocw >> 4u) & 0xFu; if (d != 0u) R[d].a = aab;\n"
    "    d = aocw & 0xFu;         if (d != 0u) R[d].a = acd;\n"
    "    d = (aocw >> 8u) & 0xFu; if (d != 0u) R[d].a = ams;\n"
    "    R[0] = vec4(0.0);\n"
    "  }\n"
    "  vec4 res;\n"
    "  if (u_final.x != 0u || u_final.y != 0u) {\n"
    "    uint f0 = u_final.x, f1 = u_final.y, ff = f1 & 0xFFu;\n"
    "    R[1] = u_fc0; R[2] = u_fc1;\n"
    "    vec3 a = ((ff & 0x40u) != 0u) ? 1.0 - R[5].rgb : R[5].rgb;\n"
    "    vec3 b = ((ff & 0x20u) != 0u) ? 1.0 - R[12].rgb : R[12].rgb;\n"
    "    R[14] = vec4(((ff & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);\n"
    "    vec3 E = rd_rgb(f1 >> 24u), F = rd_rgb(f1 >> 16u);\n"
    "    R[15] = vec4(E * F, 0.0);\n"
    "    vec3 A = rd_rgb(f0 >> 24u), B = rd_rgb(f0 >> 16u);\n"
    "    vec3 C = rd_rgb(f0 >> 8u),  D = rd_rgb(f0);\n"
    "    res = vec4(D + C * (1.0 - A) + B * A, rd_a(f1 >> 8u));\n"
    "  } else {\n"
    "    res = R[12];\n"
    "  }\n"
    "  return clamp(res, 0.0, 1.0);\n"
    "}\n"
    "void main(){\n"
    "  vec4 c;\n"
    "  if (u_rc == 0) {\n"
    "    c = v_col * texture(u_tex0, v_uv);\n"
    "  } else {\n"
    "    vec4 t0 = stage(0, v_t0, u_tex0), t1 = stage(1, v_t1, u_tex1);\n"
    "    vec4 t2 = stage(2, v_t2, u_tex2), t3 = stage(3, v_t3, u_tex3);\n"
    "    if (g_kill) discard;\n"
    "    c = rc_eval(t0, t1, t2, t3);\n"
    "  }\n"
    "  if (u_alpha.x != 0) {\n"
    "    int a = int(c.a * 255.0 + 0.5), r = u_alpha.y; bool p;\n"
    "    switch (u_alpha.x) {\n"
    "      case 0x200: p = false; break;  case 0x201: p = a <  r; break;\n"
    "      case 0x202: p = a == r; break; case 0x203: p = a <= r; break;\n"
    "      case 0x204: p = a >  r; break; case 0x205: p = a != r; break;\n"
    "      case 0x206: p = a >= r; break; default:    p = true;\n"
    "    }\n"
    "    if (!p) discard;\n"
    "  }\n"
    "  o = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[GL] shader: %s\n", log);
    }
    return s;
}

static void program_init(void)
{
    static const uint32_t white = 0xFFFFFFFFu;
    static const char *samplers[4] = { "u_tex0", "u_tex1", "u_tex2", "u_tex3" };
    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    GLint ok = 0;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(s_prog, sizeof log, NULL, log);
        fprintf(stderr, "[GL] link: %s\n", log);
    }
    glUseProgram(s_prog);
    u_surf = glGetUniformLocation(s_prog, "u_surf");
    u_texsize = glGetUniformLocation(s_prog, "u_texsize");
    u_alpha = glGetUniformLocation(s_prog, "u_alpha");
    u_rc = glGetUniformLocation(s_prog, "u_rc");
    u_tscale = glGetUniformLocation(s_prog, "u_tscale");
    u_fogc = glGetUniformLocation(s_prog, "u_fogc");
    u_cicw = glGetUniformLocation(s_prog, "u_cicw");
    u_aicw = glGetUniformLocation(s_prog, "u_aicw");
    u_cocw = glGetUniformLocation(s_prog, "u_cocw");
    u_aocw = glGetUniformLocation(s_prog, "u_aocw");
    u_f0 = glGetUniformLocation(s_prog, "u_f0");
    u_f1 = glGetUniformLocation(s_prog, "u_f1");
    u_final = glGetUniformLocation(s_prog, "u_final");
    u_fc0 = glGetUniformLocation(s_prog, "u_fc0");
    u_fc1 = glGetUniformLocation(s_prog, "u_fc1");
    u_ctl = glGetUniformLocation(s_prog, "u_ctl");
    for (int i = 0; i < 4; i++)
        glUniform1i(glGetUniformLocation(s_prog, samplers[i]), i);

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                          (void *)offsetof(Nv2aVertex, x));
    glEnableVertexAttribArray(1);       /* 0xAARRGGBB na memoria = B,G,R,A */
    glVertexAttribPointer(1, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE,
                          sizeof(Nv2aVertex), (void *)offsetof(Nv2aVertex, diffuse));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertex),
                          (void *)offsetof(Nv2aVertex, u));
    glGenBuffers(1, &s_vbo_ext);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
    for (int i = 0; i < 4; i++)
        glVertexAttribPointer(3 + (GLuint)i, 4, GL_FLOAT, GL_FALSE,
                              sizeof(Nv2aVertexExt),
                              (void *)(offsetof(Nv2aVertexExt, tex) + (size_t)i * 16));
    glVertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexExt),
                          (void *)offsetof(Nv2aVertexExt, specular));
    glVertexAttribPointer(8, 1, GL_FLOAT, GL_FALSE, sizeof(Nv2aVertexExt),
                          (void *)offsetof(Nv2aVertexExt, fog));

    glGenTextures(1, &s_white);
    glBindTexture(GL_TEXTURE_2D, s_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA,
                 GL_UNSIGNED_INT_8_8_8_8_REV, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glEnable(GL_DEPTH_CLAMP);           /* o near ja foi recortado na CPU */
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
}

static void module_env(void);

static int make_current(void)
{
    if (!s_inited) {
        module_env();
        program_init();
        s_inited = 1;
    }
    return 1;
}

static void flush_pending(void);   /* lotes: definido adiante */
static int s_trace_frames;
static FILE *s_trace_fp;            /* destino do [GLT]: stderr ou o arquivo do F9 */
#define TRACE_FP (s_trace_fp ? s_trace_fp : stderr)
static int s_no_poly_offset;        /* F11: ignora o z-bias (polygon offset) */
static int s_dbg;   /* DW4_GL_DBG: 1 sem alpha test, 2 sem blend, 4 sem depth, 8 sem combiners */

/* ── Superficies ───────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t va, w, h, pitch, bpp, clip_h;  /* do guest */
    uint32_t pw, ph;                    /* imagem da GPU (render_opts.h) */
    int wide;                           /* superficie de tela alargada */
    GLuint fbo, color;
    uint32_t zeta_va;                   /* depth anexado no momento */
} Surface;

typedef struct {
    uint32_t va, w, h;
    GLuint rb;
} Zeta;

#define MAX_SURF 64
static Surface s_surf[MAX_SURF];
static int s_nsurf;
static Zeta s_zeta[MAX_SURF];
static int s_nzeta;
static Surface *s_shown;                /* a maior superficie desenhada no frame */
static int s_shown_stale = 1;
static uint32_t s_frame;

static uint32_t pow2_ceil(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
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
     * (512x512 com clip 498x498+7+7 no lobby). */
    if ((w & (w - 1)) == 0 && h <= w && h > w / 2)
        h = w;
    else if ((w & (w - 1)) == 0 && h < w)
        h = pow2_ceil(h);

    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].va == s->color_va) { e = &s_surf[i]; break; }
    if (e && (e->w != w || e->h < h)) {       /* mudou de tamanho: recria */
        flush_pending();                      /* o lote pendente usa o FBO */
        glDeleteFramebuffers(1, &e->fbo);
        glDeleteTextures(1, &e->color);
        e->fbo = 0;
    }
    if (!e) {
        if (s_nsurf == MAX_SURF) {            /* raro: recicla a mais antiga */
            flush_pending();
            glDeleteFramebuffers(1, &s_surf[0].fbo);
            glDeleteTextures(1, &s_surf[0].color);
            if (s_shown == &s_surf[0]) s_shown = NULL;
            e = &s_surf[0];
        } else {
            e = &s_surf[s_nsurf++];
        }
        memset(e, 0, sizeof *e);
    }
    if (!e->fbo) {
        e->va = s->color_va; e->w = w; e->h = h; e->zeta_va = ~0u;
        e->wide = ro_is_screen(w, h);
        ro_extent(w, h, e->wide, &e->pw, &e->ph);
        if (ro_enhanced())
            fprintf(stderr, "[GL] superficie %08X %ux%u (clip %ux%u) -> imagem %ux%u%s\n",
                    e->va, w, h, s->width, s->height, e->pw, e->ph, e->wide ? " (tela)" : "");
        glGenTextures(1, &e->color);
        glBindTexture(GL_TEXTURE_2D, e->color);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)e->pw, (GLsizei)e->ph, 0,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &e->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, e->color, 0);
        glClearColor(0, 0, 0, 1);
        glColorMask(1, 1, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    e->pitch = s->pitch; e->bpp = bpp; e->clip_h = s->height;
    return e;
}

static void surface_bind(Surface *e, uint32_t zeta_va)
{
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
    if (e->zeta_va != zeta_va) {
        GLuint rb = 0;

        if (zeta_va) {
            Zeta *z = NULL;
            for (int i = 0; i < s_nzeta; i++)
                if (s_zeta[i].va == zeta_va && s_zeta[i].w == e->pw
                        && s_zeta[i].h == e->ph) { z = &s_zeta[i]; break; }
            if (!z) {
                if (s_nzeta == MAX_SURF) {
                    glDeleteRenderbuffers(1, &s_zeta[0].rb);
                    z = &s_zeta[0];
                } else {
                    z = &s_zeta[s_nzeta++];
                }
                z->va = zeta_va; z->w = e->pw; z->h = e->ph;
                glGenRenderbuffers(1, &z->rb);
                glBindRenderbuffer(GL_RENDERBUFFER, z->rb);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
                                      (GLsizei)e->pw, (GLsizei)e->ph);
            }
            rb = z->rb;
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                                  GL_RENDERBUFFER, rb);
        e->zeta_va = zeta_va;
    }
    glViewport(0, 0, (GLsizei)e->pw, (GLsizei)e->ph);
}

static void note_shown(Surface *e)
{
    /* Pela area da superficie, nao pelo recorte do ultimo draw (ver o mesmo
     * trecho em nv2a_vk_backend.c: a tela piscava distorcida). */
    if (!s_shown_stale && s_shown && e->w * e->h < s_shown->w * s_shown->h)
        return;
    s_shown_stale = 0;
    s_shown = e;
}

/* ── Texturas ──────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t offset, w, h, color, palette;
    uint8_t amin, amax, aavg;           /* alfa dos texels decodificados (diagnostico) */
    uint32_t first[4];                  /* 4 primeiros texels ARGB */
    GLuint tex;
    uint64_t hash;
    uint32_t checked_frame;
    int used;
} TexEntry;

#define TEX_SLOTS 2048
static TexEntry s_tex[TEX_SLOTS];
static int s_tex_count;
static uint32_t *s_decode;
static size_t s_decode_cap;
static unsigned long s_n_decode, s_n_draw, s_n_tris, s_n_rtt;

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

/* Bytes do nivel 0, pelo formato NV097 (estimativa por baixo quando o formato
 * nao e conhecido: ler alem da textura faria ela "mudar" todo frame). */
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

/* A textura e a imagem de uma superficie de tela alargada (render_opts.h)? */
static int tex_is_wide_surface(GLuint tex)
{
    if (tex == s_white)
        return 0;
    for (int i = 0; i < s_nsurf; i++)
        if (s_surf[i].color == tex && s_surf[i].fbo)
            return s_surf[i].wide;
    return 0;
}

static GLuint texture_get(const Nv2aTexture *t, const Surface *target)
{
    uint32_t va = tex_va(t->offset);
    uint32_t pal_key = 0, idx;

    /* Paleta: identificada pelo CONTEUDO, conferido a cada uso. O jogo desenha
     * muita coisa com a mesma textura de indices trocando so a paleta entre
     * um draw e outro do mesmo frame (as vezes no mesmo endereco); conferir
     * uma vez por frame deixava tudo com a primeira paleta -- cinza. */
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
    TexEntry *e = NULL;
    const uint8_t *bytes;
    size_t nbytes;
    uint64_t h = 0;

    /* render-to-texture: a imagem que a GPU desenhou naquele endereco */
    for (int i = 0; i < s_nsurf; i++) {
        if (s_surf[i].va == va && s_surf[i].fbo) {
            if (&s_surf[i] == target)
                return s_white;         /* amostrar o proprio alvo: indefinido */
            s_n_rtt++;
            return s_surf[i].color;
        }
    }
    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return s_white;

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
        for (int i = 0; i < TEX_SLOTS; i++)      /* cache cheio: recomeca */
            if (s_tex[i].used) glDeleteTextures(1, &s_tex[i].tex);
        memset(s_tex, 0, sizeof s_tex);
        s_tex_count = 0;
        e = &s_tex[idx];
    }

    if (e->used && e->checked_frame == s_frame)
        return e->tex;
    nbytes = tex_bytes(t);
    bytes = guest_bytes(va, nbytes);
    if (bytes)
        h = hash_bytes(bytes, nbytes);
    if (e->used && (h == e->hash || !bytes)) {
        e->checked_frame = s_frame;
        return e->tex;
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
                fprintf(stderr, "[GL] decode falhou: off=%08X va=%08X %ux%u "
                        "pitch=%u fmt=0x%02X levels=%u\n", t->offset, va,
                        t->width, t->height, t->pitch, t->color, t->levels);
            if (!e->used)
                return s_white;
            e->checked_frame = s_frame;
            return e->tex;
        }
    }
    if (!e->used) {
        e->used = 1;
        e->offset = va; e->w = t->width; e->h = t->height; e->color = t->color;
        e->palette = pal_key;
        glGenTextures(1, &e->tex);
        s_tex_count++;
    }
    {
        size_t npx = (size_t)t->width * t->height;
        uint64_t sum = 0;
        uint32_t lo = 255, hi = 0;
        for (size_t i = 0; i < npx; i++) {
            uint32_t a = s_decode[i] >> 24;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
            sum += a;
        }
        e->amin = (uint8_t)lo; e->amax = (uint8_t)hi; e->aavg = (uint8_t)(sum / npx);
        for (int i = 0; i < 4; i++)
            e->first[i] = i < (int)npx ? s_decode[i] : 0;
    }
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)t->width,
                 (GLsizei)t->height, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                 s_decode);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    e->hash = h;
    e->checked_frame = s_frame;
    s_n_decode++;
    return e->tex;
}

static const TexEntry *tex_entry_by_gl(GLuint tex)
{
    for (int i = 0; i < TEX_SLOTS; i++)
        if (s_tex[i].used && s_tex[i].tex == tex)
            return &s_tex[i];
    return NULL;
}

static GLint wrap_mode(uint32_t nv)
{
    switch (nv) {
    case 1:  return GL_REPEAT;
    case 2:  return GL_MIRRORED_REPEAT;
    default: return GL_CLAMP_TO_EDGE;
    }
}

/* ── Callbacks ─────────────────────────────────────────────────────────────── */

static float zmax_of(const Nv2aRenderState *rs)
{
    return rs->depth_max > 65536.0f ? 16777215.0f : 65535.0f;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Surface *e;
    GLbitfield mask = 0;

    if (!make_current())
        return;
    flush_pending();
    e = surface_get(s);
    surface_bind(e, rs->zeta_va);
    glDisable(GL_SCISSOR_TEST);
    if (s_trace_frames > 0)
        fprintf(TRACE_FP, "[GLT] f%u clear surf=%08X %ux%u flags=%02X argb=%08X zs=%08X zeta=%08X\n",
                s_frame, e->va, e->w, e->h, flags, argb, zstencil, rs->zeta_va);
    if (flags & 0xF0) {
        glColorMask(!!(flags & 0x10), !!(flags & 0x20), !!(flags & 0x40),
                    !!(flags & 0x80));
        glClearColor(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                     (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
        note_shown(e);
    }
    if (rs->zeta_va) {
        if (flags & 0x1) {
            float d = zmax_of(rs) > 65536.0f ? (float)(zstencil >> 8) / 16777215.0f
                                             : (float)(zstencil & 0xFFFF) / 65535.0f;
            glDepthMask(GL_TRUE);
            glClearDepth(d);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if (flags & 0x2) {
            glStencilMask(0xFF);
            glClearStencil((GLint)(zstencil & 0xFF));
            mask |= GL_STENCIL_BUFFER_BIT;
        }
    }
    if (mask)
        glClear(mask);
}

static GLenum blend_eq(uint32_t nv)
{
    switch (nv) {
    case 0x800A: return GL_FUNC_SUBTRACT;
    case 0x800B: return GL_FUNC_REVERSE_SUBTRACT;
    case 0x8007: return GL_MIN;
    case 0x8008: return GL_MAX;
    default:     return GL_FUNC_ADD;     /* 0x8006 e as variantes "signed" */
    }
}

static GLenum blend_factor(uint32_t nv, GLenum fallback)
{
    if (nv <= 1 || (nv >= 0x300 && nv <= 0x308) || (nv >= 0x8001 && nv <= 0x8004))
        return (GLenum)nv;
    return fallback;
}

static GLenum compare_func(uint32_t nv)
{
    return (nv >= 0x200 && nv <= 0x207) ? (GLenum)nv : GL_ALWAYS;
}

static GLenum stencil_op(uint32_t nv)
{
    switch (nv) {
    case 0: case 0x1E00: case 0x1E01: case 0x1E02: case 0x1E03:
    case 0x150A: case 0x8507: case 0x8508:
        return (GLenum)nv;
    default:
        return GL_KEEP;
    }
}

/* ── Lotes ─────────────────────────────────────────────────────────────────── *
 * Tudo o que decide como um draw sai. Draws consecutivos com a mesma chave
 * sao acumulados e saem num unico glDrawArrays. */
typedef struct {
    Surface *surf;
    Nv2aRenderState rs;
    int has_ext;
    GLuint tex[4];
    GLint wrap_s[4], wrap_t[4], filter[4];
    float tscale[4][2];
    float texsize[2];                   /* caminho simples */
    Nv2aCombiner rc;
    float fogc[4];
    uint32_t clip_mode;
    uint32_t sc[4];                     /* recorte do guest (x, y, l, a); zeros = inteira */
} DrawKey;

static double s_trace_at = -1.0;
static int s_trace_is_f9;
static void f9_capture(void);
static DrawKey s_key;
static int s_key_valid;
static Nv2aVertex *s_pv;
static Nv2aVertexExt *s_pe;
static uint32_t s_pcount, s_pcap;
static unsigned long s_n_flush;
#define PENDING_MAX 196608u

static GLint filter_mode(uint32_t f)
{
    uint32_t mag = (f >> 24) & 0xF, min = (f >> 16) & 0xFF;
    if (s_dbg & 16)
        return GL_NEAREST;
    return (mag == 2 || min == 2 || min == 4 || min == 6) ? GL_LINEAR : GL_NEAREST;
}

static void flush_pending(void)
{
    const DrawKey *k = &s_key;
    const Nv2aRenderState *rs = &k->rs;

    if (!s_pcount || !s_key_valid) {
        s_pcount = 0;
        return;
    }
    /* 16:9: comprime em x, com o lote inteiro a vista (render_opts.h). */
    if (k->surf->wide)
        ro_squeeze(s_pv, s_pcount, (float)k->surf->w, (float)ro_display_rows(k->surf->h),
                   k->tex[0] != s_white || k->tex[1] != s_white
                   || k->tex[2] != s_white || k->tex[3] != s_white,
                   tex_is_wide_surface(k->tex[0]) || tex_is_wide_surface(k->tex[1])
                   || tex_is_wide_surface(k->tex[2]) || tex_is_wide_surface(k->tex[3]),
                   k->sc);
    surface_bind(k->surf, rs->zeta_va);
    glUseProgram(s_prog);
    glUniform3f(u_surf, (float)k->surf->w, (float)k->surf->h, zmax_of(rs));
    glUniform2f(u_texsize, k->texsize[0], k->texsize[1]);
    glUniform2i(u_alpha, (rs->alpha_test_enable && !(s_dbg & 1)) ? (GLint)rs->alpha_func : 0,
                (GLint)rs->alpha_ref);
    glUniform1i(u_rc, (s_dbg & 8) ? 0 : k->has_ext);

    for (int i = 0; i < 4; i++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, k->tex[i]);
        if (k->tex[i] != s_white) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, k->wrap_s[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, k->wrap_t[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, k->filter[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, k->filter[i]);
        }
    }
    glActiveTexture(GL_TEXTURE0);

    if (k->has_ext) {
        static Nv2aCombiner sent;
        static int sent_valid;
        const Nv2aCombiner *rc = &k->rc;

        glUniform2fv(u_tscale, 4, &k->tscale[0][0]);
        glUniform4fv(u_fogc, 1, k->fogc);
        glUniform3ui(u_ctl, rc->control, rc->stage_program, k->clip_mode);
        if (!sent_valid || memcmp(&sent, rc, sizeof sent) != 0) {
            float f0[8][4], f1[8][4], c0[4], c1[4];

            for (int i = 0; i < 8; i++) {
                nv2a_rc_unpack(rc->factor0[i], f0[i]);
                nv2a_rc_unpack(rc->factor1[i], f1[i]);
            }
            nv2a_rc_unpack(rc->final_c0, c0);
            nv2a_rc_unpack(rc->final_c1, c1);
            glUniform1uiv(u_cicw, 8, rc->color_icw);
            glUniform1uiv(u_aicw, 8, rc->alpha_icw);
            glUniform1uiv(u_cocw, 8, rc->color_ocw);
            glUniform1uiv(u_aocw, 8, rc->alpha_ocw);
            glUniform4fv(u_f0, 8, &f0[0][0]);
            glUniform4fv(u_f1, 8, &f1[0][0]);
            glUniform2ui(u_final, rc->final0, rc->final1);
            glUniform4fv(u_fc0, 1, c0);
            glUniform4fv(u_fc1, 1, c1);
            sent = *rc;
            sent_valid = 1;
        }
    }

    if (rs->blend_enable && !(s_dbg & 2)) {
        glEnable(GL_BLEND);
        glBlendEquation(blend_eq(rs->blend_eq));
        glBlendFunc(blend_factor(rs->blend_src, GL_ONE),
                    blend_factor(rs->blend_dst, GL_ZERO));
        glBlendColor(((rs->blend_color >> 16) & 0xFF) / 255.0f,
                     ((rs->blend_color >> 8) & 0xFF) / 255.0f,
                     (rs->blend_color & 0xFF) / 255.0f,
                     (rs->blend_color >> 24) / 255.0f);
    } else {
        glDisable(GL_BLEND);
    }

    if (rs->zeta_va && (rs->depth_test_enable || rs->depth_write) && !(s_dbg & 4)) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(rs->depth_test_enable ? compare_func(rs->depth_func) : GL_ALWAYS);
        glDepthMask(rs->depth_write ? GL_TRUE : GL_FALSE);
    } else {
        glDisable(GL_DEPTH_TEST);
    }

    if (rs->zeta_va && rs->stencil_enable) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(compare_func(rs->stencil_func), (GLint)rs->stencil_ref,
                      rs->stencil_func_mask);
        glStencilOp(stencil_op(rs->stencil_fail), stencil_op(rs->stencil_zfail),
                    stencil_op(rs->stencil_zpass));
        glStencilMask(rs->stencil_write_mask);
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    glColorMask(!!(rs->color_mask & 0x00010000), !!(rs->color_mask & 0x00000100),
                !!(rs->color_mask & 0x00000001), !!(rs->color_mask & 0x01000000));

    /* z-bias: os valores da NV2A sao os do glPolygonOffset (como no xemu). */
    if (rs->poly_offset_enable && !s_no_poly_offset
            && rs->poly_offset_factor == rs->poly_offset_factor
            && rs->poly_offset_bias == rs->poly_offset_bias) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(rs->poly_offset_factor, rs->poly_offset_bias);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (s_cull_mode && rs->cull_enable
            && (rs->cull_face == 0x404 || rs->cull_face == 0x405)
            && (rs->front_face == 0x900 || rs->front_face == 0x901)) {
        /* A NV2A define horario/anti-horario na tela, com y para baixo. Aqui
         * a linha 0 do FBO e a linha 0 da superficie (y da tela = y de janela
         * do GL, que cresce para cima), ou seja, a imagem esta espelhada na
         * vertical dentro do GL e o sentido de giro se inverte. Por isso o
         * modo normal troca CW <-> CCW; o modo 2 (F5) e o valor cru, para
         * depuracao. Depende de RECOMP_STRIP_ALT_WINDING=1, senao metade dos
         * triangulos de cada tira chega com o giro trocado (modelos
         * "salpicados" na selecao de Digimon). */
        GLenum ff = (GLenum)rs->front_face;
        if (s_cull_mode == 1)
            ff = ff == GL_CW ? GL_CCW : GL_CW;
        glEnable(GL_CULL_FACE);
        glFrontFace(ff);
        glCullFace((GLenum)rs->cull_face);
    } else {
        glDisable(GL_CULL_FACE);
    }

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_pcount * sizeof(Nv2aVertex)),
                 s_pv, GL_STREAM_DRAW);
    if (k->has_ext) {
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo_ext);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s_pcount * sizeof(Nv2aVertexExt)),
                     s_pe, GL_STREAM_DRAW);
        for (GLuint i = 3; i <= 8; i++)
            glEnableVertexAttribArray(i);
    } else {
        for (GLuint i = 3; i <= 8; i++)
            glDisableVertexAttribArray(i);
    }
    {
        /* Recorte do guest (sub-viewport: o minimapa). A linha 0 do FBO e a
         * linha 0 da superficie, entao o y vale direto. */
        GLint r[4];

        if (ro_scissor(k->sc, k->surf->wide, k->surf->w, k->surf->h, k->surf->pw,
                       k->surf->ph, r)) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(r[0], r[1], r[2], r[3]);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)s_pcount);
    glDisable(GL_SCISSOR_TEST);
    s_n_flush++;
    s_pcount = 0;
}

/* ── Sonda do pixel central (so durante a captura do F9) ───────────────────
 * Para cada draw que cobre o centro da tela: profundidade e cor daquele pixel
 * antes e depois. Mostra quem escondeu quem. */
static int s_skip_depth_only;          /* F10: ignora draws com escrita de cor desligada */
static GLuint s_query;                 /* captura F9: pixels pintados por draw */
static int s_query_open;
static int s_atlas_dumped;
static unsigned s_q_seq, s_q_col, s_q_dep;   /* ordinais por frame: cor / so-profundidade */

static int tri_covers(const Nv2aVertex *v, float px, float py, float *z)
{
    float w0 = (v[1].x - v[0].x) * (py - v[0].y) - (v[1].y - v[0].y) * (px - v[0].x);
    float w1 = (v[2].x - v[1].x) * (py - v[1].y) - (v[2].y - v[1].y) * (px - v[1].x);
    float w2 = (v[0].x - v[2].x) * (py - v[2].y) - (v[0].y - v[2].y) * (px - v[2].x);
    float area = w0 + w1 + w2;

    if (!((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
            || area == 0.0f)
        return 0;
    /* w1 e oposto a v0, w2 a v1, w0 a v2 */
    *z = (w1 * v[0].z + w2 * v[1].z + w0 * v[2].z) / area;
    return 1;
}

static void probe_read(const Surface *e, int px, int py, float *depth, uint32_t *argb)
{
    *depth = -1.0f;
    *argb = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
    glReadPixels(px, py, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, argb);
    if (e->zeta_va && e->zeta_va != ~0u)
        glReadPixels(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    DrawKey key;
    Surface *e;
    int probe = 0;
    float probe_z = 0.0f, d0 = 0.0f;
    uint32_t c0 = 0;
    static unsigned draw_no;

    /* F10, ou o ultimo frame de uma captura F9 (quadro de comparacao "B"). */
    if ((s_skip_depth_only || (s_trace_fp && s_trace_frames == 1))
            && !(b->state->color_mask & 0x01010101u))
        return;

    if (!b->count || !make_current())
        return;
    /* A superficie pode ser recriada aqui; o lote pendente aponta para ela. */
    e = surface_get(s);

    memset(&key, 0, sizeof key);
    key.surf = e;
    key.rs = *b->state;
    /* Recorte menor que a superficie (sub-viewport: o minimapa). */
    if (s->width && s->height && (s->width < e->w || s->height < e->h)) {
        key.sc[0] = s->clip_x; key.sc[1] = s->clip_y;
        key.sc[2] = s->width;  key.sc[3] = s->height;
    }
    key.texsize[0] = key.texsize[1] = 1.0f;
    for (int i = 0; i < 4; i++) {
        key.tex[i] = s_white;
        key.tscale[i][0] = key.tscale[i][1] = 1.0f;
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
                continue;               /* estagio sem textura: le branco */
            key.tex[i] = texture_get(t, e);
            if (key.tex[i] == s_white)
                continue;
            key.wrap_s[i] = wrap_mode(t->addr_u);
            key.wrap_t[i] = wrap_mode(t->addr_v);
            key.filter[i] = filter_mode(t->filter);
            if (!x->tex_normalised[i]) {
                key.tscale[i][0] = 1.0f / (float)t->width;
                key.tscale[i][1] = 1.0f / (float)t->height;
            }
        }
    } else if (b->texture) {
        key.tex[0] = texture_get(b->texture, e);
        if (key.tex[0] != s_white) {
            key.texsize[0] = (float)b->texture->width;
            key.texsize[1] = (float)b->texture->height;
            key.wrap_s[0] = wrap_mode(b->texture->addr_u);
            key.wrap_t[0] = wrap_mode(b->texture->addr_v);
            key.filter[0] = filter_mode(b->texture->filter);
        }
    }

    if (s_trace_frames > 0) {
        const Nv2aRenderState *r = b->state;
        fprintf(TRACE_FP, "[GLT] f%u draw surf=%08X %ux%u clip=%ux%u zeta=%08X tris=%u ext=%d"
                " blend=%u(%X,%X,eq %X) atest=%u(%X,%u) z=%u/%u(%X) cmask=%08X stencil=%u",
                s_frame, e->va, e->w, e->h, s->width, s->height, r->zeta_va,
                b->count / 3, b->ext != NULL, r->blend_enable, r->blend_src,
                r->blend_dst, r->blend_eq, r->alpha_test_enable, r->alpha_func,
                r->alpha_ref, r->depth_test_enable, r->depth_write, r->depth_func,
                r->color_mask, r->stencil_enable);
        if (b->ext) {
            const Nv2aCombiner *c = &b->ext->rc;
            fprintf(TRACE_FP, " prog=%05X ctl=%X cicw=%08X cocw=%08X aicw=%08X aocw=%08X"
                    " f0=%08X f1=%08X fin=%08X,%08X fc=%08X,%08X spec=(%.2f,%.2f,%.2f,%.2f)"
                    " fog=%.2f", c->stage_program, c->control, c->color_icw[0],
                    c->color_ocw[0], c->alpha_icw[0], c->alpha_ocw[0], c->factor0[0],
                    c->factor1[0], c->final0, c->final1, c->final_c0, c->final_c1,
                    b->ext->vertices[0].specular[0], b->ext->vertices[0].specular[1],
                    b->ext->vertices[0].specular[2], b->ext->vertices[0].specular[3],
                    b->ext->vertices[0].fog);
            for (int i = 0; i < 4; i++)
                if ((b->ext->rc.stage_program >> (i * 5)) & 0x1F)
                    fprintf(TRACE_FP, " t%d[%08X %ux%u f%02X v%u %s]", i,
                            tex_va(b->ext->tex[i].offset), b->ext->tex[i].width,
                            b->ext->tex[i].height, b->ext->tex[i].color,
                            b->ext->tex_valid[i],
                            key.tex[i] == s_white ? "WHITE" : "ok");
        } else if (b->texture) {
            fprintf(TRACE_FP, " tex[%08X %ux%u f%02X %s]", tex_va(b->texture->offset),
                    b->texture->width, b->texture->height, b->texture->color,
                    key.tex[0] == s_white ? "WHITE" : "ok");
        }
        if (b->ext && b->ext->tex[0].palette) {
            const uint32_t *pal = (const uint32_t *)guest_bytes(b->ext->tex[0].palette, 1024);
            const uint8_t *ix = guest_bytes(tex_va(b->ext->tex[0].offset), 16);
            fprintf(TRACE_FP, " filt=%08X wrap=%u,%u pal@%08X", b->ext->tex[0].filter,
                    b->ext->tex[0].addr_u, b->ext->tex[0].addr_v, b->ext->tex[0].palette);
            if (pal)
                fprintf(TRACE_FP, "={%08X,%08X,%08X,%08X..%08X}", pal[0], pal[1], pal[2],
                        pal[3], pal[255]);
            if (ix)
                fprintf(TRACE_FP, " ix=%02X%02X%02X%02X", ix[0], ix[1], ix[2], ix[3]);
            fprintf(TRACE_FP, " uv0=(%.3f,%.3f,%.3f,%.3f)", b->ext->vertices[0].tex[0][0],
                    b->ext->vertices[0].tex[0][1], b->ext->vertices[0].tex[0][2],
                    b->ext->vertices[0].tex[0][3]);
        }
        fprintf(TRACE_FP, " v0=(%.1f,%.1f,%.1f,%.3f) col=%08X\n", b->vertices[0].x,
                b->vertices[0].y, b->vertices[0].z, b->vertices[0].rhw,
                b->vertices[0].diffuse);
    }
    draw_no++;
    if (s_trace_frames > 0) {
        /* Um draw por lote durante a captura, com contagem de pixels pintados
         * (GL_SAMPLES_PASSED) e a caixa na tela. Comparando o quadro A (normal)
         * com o B (sem os draws so de profundidade), quem pintou 0 em A e algo
         * em B e o que esta sendo escondido. */
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, z0 = 1e30f, z1 = -1e30f;

        for (uint32_t i = 0; i < b->count; i++) {
            const Nv2aVertex *v = &b->vertices[i];
            if (v->x < x0) x0 = v->x;
            if (v->x > x1) x1 = v->x;
            if (v->y < y0) y0 = v->y;
            if (v->y > y1) y1 = v->y;
            if (v->z < z0) z0 = v->z;
            if (v->z > z1) z1 = v->z;
        }
        flush_pending();
        if (!s_query)
            glGenQueries(1, &s_query);
        s_query_open = 1;
        s_q_seq = (b->state->color_mask & 0x01010101u) ? ++s_q_col : ++s_q_dep;
        fprintf(TRACE_FP, "[Q] f%u %c %c=%u surf=%08X bbox=(%.0f,%.0f,%.0f,%.0f) "
                "zn=(%.6f,%.6f) ", s_frame,
                (s_trace_fp && s_trace_frames == 1) ? 'B' : 'A',
                (b->state->color_mask & 0x01010101u) ? 'n' : 'd', s_q_seq, e->va,
                (double)x0, (double)y0, (double)x1, (double)y1,
                (double)(z0 / zmax_of(b->state)), (double)(z1 / zmax_of(b->state)));
    }
    /* a sonda le o pixel (320,240) do guest: so na resolucao original */
    if (s_trace_frames > 0 && e->w == 640 && s->height >= 400 && !ro_enhanced()) {
        for (uint32_t i = 0; i + 2 < b->count && !probe; i += 3)
            probe = tri_covers(&b->vertices[i], 320.0f, 240.0f, &probe_z);
        if (probe) {
            flush_pending();
            surface_bind(e, b->state->zeta_va);
            probe_read(e, 320, 240, &d0, &c0);
        }
    }
    if (!s_key_valid || memcmp(&key, &s_key, sizeof key) != 0
            || s_pcount + b->count > PENDING_MAX) {
        flush_pending();
        s_key = key;
        s_key_valid = 1;
    }
    if (s_pcount + b->count > s_pcap) {
        uint32_t cap = s_pcap ? s_pcap : 16384;
        while (cap < s_pcount + b->count)
            cap *= 2;
        s_pv = realloc(s_pv, cap * sizeof *s_pv);
        s_pe = realloc(s_pe, cap * sizeof *s_pe);
        s_pcap = (s_pv && s_pe) ? cap : 0;
        if (!s_pcap) {
            s_pcount = 0;
            return;
        }
    }
    memcpy(s_pv + s_pcount, b->vertices, b->count * sizeof *s_pv);
    if (b->ext)
        memcpy(s_pe + s_pcount, b->ext->vertices, b->count * sizeof *s_pe);
    s_pcount += b->count;
    note_shown(e);
    s_n_draw++;
    s_n_tris += b->count / 3;
    if (s_query_open) {
        const Nv2aRenderState *r = b->state;
        GLuint samples = 0;

        glBeginQuery(GL_SAMPLES_PASSED, s_query);
        flush_pending();
        glEndQuery(GL_SAMPLES_PASSED);
        glGetQueryObjectuiv(s_query, GL_QUERY_RESULT, &samples);
        s_query_open = 0;
        fprintf(TRACE_FP, "samples=%u tris=%u cmask=%08X ztest=%u zwrite=%u func=%X "
                "blend=%u(%X,%X) atest=%u(%X,%u) zbias=%u(%g,%g) cull=%u(%X,%X) "
                "tex=%08X %ux%u f%02X pal=%08X col=%08X ext=%d\n",
                samples, b->count / 3, r->color_mask, r->depth_test_enable,
                r->depth_write, r->depth_func, r->blend_enable, r->blend_src,
                r->blend_dst, r->alpha_test_enable, r->alpha_func, r->alpha_ref,
                r->poly_offset_enable, (double)r->poly_offset_factor,
                (double)r->poly_offset_bias, r->cull_enable, r->cull_face, r->front_face,
                b->ext ? tex_va(b->ext->tex[0].offset) : (b->texture ? tex_va(b->texture->offset) : 0),
                b->ext ? b->ext->tex[0].width : (b->texture ? b->texture->width : 0),
                b->ext ? b->ext->tex[0].height : (b->texture ? b->texture->height : 0),
                b->ext ? b->ext->tex[0].color : (b->texture ? b->texture->color : 0),
                b->ext ? b->ext->tex[0].palette : (b->texture ? b->texture->palette : 0),
                b->vertices[0].diffuse, b->ext != NULL);
        if (b->ext) {
            /* O que decide o teste de alfa: o alfa da textura do estagio 0 na
             * regiao amostrada, a paleta crua e os combiners de alfa. */
            const Nv2aBatchExt *x = b->ext;
            const TexEntry *te = tex_entry_by_gl(s_key.tex[0]);
            const uint32_t *pal = x->tex[0].palette
                ? (const uint32_t *)guest_bytes(x->tex[0].palette, 1024) : NULL;
            const uint8_t *ix = guest_bytes(tex_va(x->tex[0].offset), 8);
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;

            for (uint32_t i = 0; i < b->count; i++) {
                const float *tc = x->vertices[i].tex[0];
                float q = tc[3] != 0.0f ? tc[3] : 1.0f;
                if (tc[0] / q < u0) u0 = tc[0] / q;
                if (tc[0] / q > u1) u1 = tc[0] / q;
                if (tc[1] / q < v0) v0 = tc[1] / q;
                if (tc[1] / q > v1) v1 = tc[1] / q;
            }
            fprintf(TRACE_FP, "[QX] uv=(%.3f,%.3f)-(%.3f,%.3f) norm=%u prog=%05X aicw=%08X "
                    "aocw=%08X cicw=%08X fin=%08X,%08X ctl=%X f0=%08X vA=%02X",
                    (double)u0, (double)v0, (double)u1, (double)v1, x->tex_normalised[0],
                    x->rc.stage_program, x->rc.alpha_icw[0], x->rc.alpha_ocw[0],
                    x->rc.color_icw[0], x->rc.final0, x->rc.final1, x->rc.control,
                    x->rc.factor0[0], b->vertices[0].diffuse >> 24);
            if (te)
                fprintf(TRACE_FP, " texA=min%u/avg%u/max%u texels=%08X,%08X,%08X,%08X",
                        te->amin, te->aavg, te->amax, te->first[0], te->first[1],
                        te->first[2], te->first[3]);
            else
                fprintf(TRACE_FP, " tex=%s", s_key.tex[0] == s_white ? "BRANCA" : "superficie");
            /* Texturas pequenas (mapas/atlas de amostra unica): redecodifica e
             * mostra o alfa no ponto amostrado -- texel exato, os 4 vizinhos
             * que um filtro linear mistura, e o resultado dessa mistura. */
            if (x->tex_valid[0] && x->tex[0].width * x->tex[0].height <= 4096
                    && x->tex[0].width && x->tex_normalised[0]) {
                static uint32_t px[4096];
                uint32_t w = x->tex[0].width, h = x->tex[0].height;

                if (nv2a_backend_decode_texture(&x->tex[0], px)) {
                    float fu = (u0 + u1) * 0.5f * (float)w, fv = (v0 + v1) * 0.5f * (float)h;
                    int iu = (int)floorf(fu), iv = (int)floorf(fv);
                    int bu = (int)floorf(fu - 0.5f), bv = (int)floorf(fv - 0.5f);
                    float wu = fu - 0.5f - (float)bu, wv = fv - 0.5f - (float)bv;
                    uint32_t a[4];
                    float lin;

                    for (int k = 0; k < 4; k++) {
                        int xx = (bu + (k & 1)) % (int)w, yy = (bv + (k >> 1)) % (int)h;
                        if (xx < 0) xx += (int)w;
                        if (yy < 0) yy += (int)h;
                        a[k] = px[(size_t)yy * w + xx] >> 24;
                    }
                    lin = ((float)a[0] * (1 - wu) + (float)a[1] * wu) * (1 - wv)
                        + ((float)a[2] * (1 - wu) + (float)a[3] * wu) * wv;
                    iu = ((iu % (int)w) + (int)w) % (int)w;
                    iv = ((iv % (int)h) + (int)h) % (int)h;
                    fprintf(TRACE_FP, " amostra=(%.2f,%.2f) aNearest=%u aViz=%u,%u,%u,%u "
                            "aLinear=%.1f", (double)fu, (double)fv,
                            px[(size_t)iv * w + iu] >> 24, a[0], a[1], a[2], a[3], (double)lin);
                    if (!s_atlas_dumped && s_trace_fp && w == 32 && h == 32
                            && !(b->state->color_mask & 0x01010101u)) {
                        /* o mapa inteiro, alfa em hexadecimal, uma linha por linha */
                        s_atlas_dumped = 1;
                        fprintf(TRACE_FP, "\n[ATLAS] tex=%08X %ux%u alfa:\n",
                                tex_va(x->tex[0].offset), w, h);
                        for (uint32_t yy = 0; yy < h; yy++) {
                            fprintf(TRACE_FP, "[ATLAS] %2u ", yy);
                            for (uint32_t xx = 0; xx < w; xx++)
                                fprintf(TRACE_FP, "%02X", px[(size_t)yy * w + xx] >> 24);
                            fprintf(TRACE_FP, "\n");
                        }
                        fprintf(TRACE_FP, "[QX+]");
                    }
                }
            }
            if (ix)
                fprintf(TRACE_FP, " ix=%02X%02X%02X%02X%02X%02X%02X%02X", ix[0], ix[1],
                        ix[2], ix[3], ix[4], ix[5], ix[6], ix[7]);
            if (pal)
                fprintf(TRACE_FP, " pal[0..3]=%08X,%08X,%08X,%08X pal[%u]=%08X pal[255]=%08X",
                        pal[0], pal[1], pal[2], pal[3], ix ? ix[0] : 0,
                        pal[ix ? ix[0] : 0], pal[255]);
            fprintf(TRACE_FP, "\n");
        }
    }
    if (probe) {
        const Nv2aRenderState *r = b->state;
        float d1;
        uint32_t c1;

        flush_pending();
        probe_read(e, 320, 240, &d1, &c1);
        fprintf(TRACE_FP, "[PROBE] f%u #%u z=%.1f (%.6f) depth %.6f -> %.6f%s cor %08X -> %08X%s"
                " | ztest=%u zwrite=%u func=%X cmask=%08X blend=%u(%X,%X) atest=%u(%X,%u)"
                " zbias=%u(%g,%g) tris=%u tex=%08X %ux%u f%02X col=%08X\n",
                s_frame, draw_no, probe_z, probe_z / zmax_of(r), d0, d1,
                d1 != d0 ? " ESCREVEU-Z" : "", c0, c1, c1 != c0 ? " MUDOU-COR" : "",
                r->depth_test_enable, r->depth_write, r->depth_func, r->color_mask,
                r->blend_enable, r->blend_src, r->blend_dst, r->alpha_test_enable,
                r->alpha_func, r->alpha_ref, r->poly_offset_enable,
                (double)r->poly_offset_factor, (double)r->poly_offset_bias, b->count / 3,
                b->ext ? tex_va(b->ext->tex[0].offset) : (b->texture ? tex_va(b->texture->offset) : 0),
                b->ext ? b->ext->tex[0].width : (b->texture ? b->texture->width : 0),
                b->ext ? b->ext->tex[0].height : (b->texture ? b->texture->height : 0),
                b->ext ? b->ext->tex[0].color : (b->texture ? b->texture->color : 0),
                b->vertices[0].diffuse);
    }
}

static uint32_t s_last_va, s_last_pitch;    /* ultimo frame trazido ao guest */

/* Janela do toolkit: recebe um frame de qualquer tamanho (patch 4.10). */
extern void xbox_FramebufferWindowPresentPixels(const void *pixels, uint32_t w,
                                                uint32_t h, uint32_t pitch);
static uint8_t *s_host_frame;               /* resolucao interna / widescreen */
static size_t s_host_cap;

/* Antes do snapshot da janela: traz o frame pronto para a memoria do guest. */
static void be_present_sync(void)
{
    Surface *e;
    uint8_t *dst;
    uint32_t rows;

    if (!s_inited)
        return;
    flush_pending();
    e = s_shown;
    if (!e || !e->fbo)
        return;
    rows = e->clip_h && e->clip_h < e->h ? e->clip_h : e->h;
    if (ro_enhanced()) {
        /* Resolucao interna / widescreen: o frame nao cabe nos 640x480 do
         * guest. Vai direto para a janela e para as capturas. A linha 0 do
         * FBO e a linha 0 da superficie, entao sai de cima para baixo. */
        uint32_t prow = (uint32_t)((float)ro_display_rows(e->h) * ro_scale + 0.5f);
        size_t need;

        if (prow > e->ph) prow = e->ph;
        need = (size_t)e->pw * prow * 4;
        if (need > s_host_cap) {
            dw4_frame_note_host(NULL, 0, 0, 0);
            free(s_host_frame);
            s_host_frame = malloc(need);
            s_host_cap = s_host_frame ? need : 0;
            if (!s_host_frame)
                return;
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadPixels(0, 0, (GLsizei)e->pw, (GLsizei)prow, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, s_host_frame);
        xbox_FramebufferWindowPresentPixels(s_host_frame, e->pw, prow, e->pw * 4);
        dw4_frame_note_host(s_host_frame, e->pw, prow, e->pw * 4);
        s_last_va = 0;
        return;
    }
    if (e->va >= CONTIG_BASE) {
        if ((uint64_t)e->va + (uint64_t)e->pitch * rows
                > (uint64_t)CONTIG_BASE + CONTIG_SIZE)
            return;
    } else if ((uint64_t)e->va + (uint64_t)e->pitch * rows > xbox_GetMappedSize()) {
        return;
    }
    dst = (uint8_t *)xbox_GetMemoryOffset() + e->va;

    glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
    if (e->bpp == 4) {
        glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(e->pitch / 4));
        glReadPixels(0, 0, (GLsizei)e->w, (GLsizei)rows, GL_BGRA,
                     GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    } else {                                    /* R5G6B5 */
        glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)(e->pitch / 2));
        glReadPixels(0, 0, (GLsizei)e->w, (GLsizei)rows, GL_RGB,
                     GL_UNSIGNED_SHORT_5_6_5, dst);
    }
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    s_last_va = e->va;
    s_last_pitch = e->bpp == 4 ? e->pitch : 0;
}

/* ── Teclas de diagnostico (janela do jogo em foco) ────────────────────────
 *   F5  culling: normal -> desligado -> invertido
 *   F6  teste de alfa liga/desliga        F7  teste de profundidade liga/desliga
 *   F8  combiners liga/desliga (desligado = texel x difusa)
 *   F9  grava build/shots/f9-NNN.bmp e, em build/shots/f9-NNN.txt, a lista de
 *       todos os draws dos 2 frames seguintes, com a sonda [PROBE] do pixel
 *       central (320,240): profundidade e cor antes/depois de cada draw que
 *       o cobre -- ponha o objeto sumido no CENTRO da tela antes de apertar
 *   F10 ignora os draws so de profundidade (escrita de cor desligada)
 *   F11 ignora o z-bias (polygon offset)
 * Para quem esta jogando achar na hora qual estado esconde um objeto. */
extern int xbox_FramebufferKeyDown(int vk);

/* F9 (ou DW4_GL_F9_AT=<s>): captura + rastreio. O arquivo traz o resto do
 * frame corrente, um frame A inteiro (normal) e um frame B inteiro (sem os
 * draws so de profundidade); scripts/f9_diff.py compara A e B. */
static void f9_capture(void)
{
    const char *dir = getenv("DW4_SHOT_DIR");
    char path[512];
    int seq = 0;

    if (s_trace_fp)
        return;
    if (!dir || !*dir)
        dir = "build/shots";
    for (; seq < 1000; seq++) {             /* proximo numero livre */
        FILE *t;
        snprintf(path, sizeof path, "%s/f9-%03d.txt", dir, seq);
        t = fopen(path, "r");
        if (!t)
            break;
        fclose(t);
    }
    s_trace_fp = fopen(path, "w");
    if (!s_trace_fp) {
        fprintf(stderr, "[GL] F9: nao criou %s\n", path);
        return;
    }
    fprintf(s_trace_fp, "# estado: cull=%d dbg=%d skip_depth_only=%d no_zbias=%d\n",
            s_cull_mode, s_dbg, s_skip_depth_only, s_no_poly_offset);
    s_trace_frames = 3;                     /* este flip + frame A + frame B */
    s_atlas_dumped = 0;
    snprintf(path, sizeof path, "%s/f9-%03d.bmp", dir, seq);
    dw4_screenshot_save(path);
    fprintf(stderr, "[GL] F9: captura em %s/f9-%03d.{bmp,txt}\n", dir, seq);
}

static void hotkeys_poll(void)
{
    static int prev[8];
    static const char *cull_name[3] = { "DESLIGADO", "normal", "GIRO CRU" };

    for (int i = 0; i < 8; i++) {
        int down = xbox_FramebufferKeyDown(0x74 + i);      /* VK_F5.. */
        int edge = down && !prev[i];

        prev[i] = down;
        if (!edge)
            continue;
        switch (i) {
        case 0:
            s_cull_mode = s_cull_mode == 1 ? 0 : (s_cull_mode == 0 ? 2 : 1);
            fprintf(stderr, "[GL] F5: culling %s\n", cull_name[s_cull_mode]);
            break;
        case 1:
            s_dbg ^= 1;
            fprintf(stderr, "[GL] F6: teste de alfa %s\n", (s_dbg & 1) ? "DESLIGADO" : "normal");
            break;
        case 2:
            s_dbg ^= 4;
            fprintf(stderr, "[GL] F7: teste de profundidade %s\n", (s_dbg & 4) ? "DESLIGADO" : "normal");
            break;
        case 3:
            s_dbg ^= 8;
            fprintf(stderr, "[GL] F8: combiners %s\n", (s_dbg & 8) ? "DESLIGADOS" : "normais");
            break;
        case 7:
            dw4_frame_ring_dump();      /* F12: caixa-preta, os ultimos ~4 s de quadros */
            break;
        case 5:
            s_skip_depth_only ^= 1;
            fprintf(stderr, "[GL] F10: draws so de profundidade %s\n",
                    s_skip_depth_only ? "IGNORADOS" : "normais");
            break;
        case 6:
            s_no_poly_offset ^= 1;
            fprintf(stderr, "[GL] F11: z-bias (polygon offset) %s\n",
                    s_no_poly_offset ? "IGNORADO" : "normal");
            break;
        default:
            f9_capture();
            break;
        }
    }
}

static void be_flip(void)
{
    if (s_inited)
        flush_pending();
    dw4_frame_note(s_last_va, s_last_pitch);
    s_shown_stale = 1;
    s_frame++;
    s_q_col = s_q_dep = 0;
    if (s_trace_frames > 0 && --s_trace_frames == 0 && s_trace_fp) {
        fclose(s_trace_fp);
        s_trace_fp = NULL;
    }
    hotkeys_poll();
    if (s_trace_at >= 0.0) {
        static struct timespec t0;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!t0.tv_sec)
            t0 = now;
        if ((double)(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9
                >= s_trace_at) {
            if (s_trace_is_f9)
                f9_capture();
            else
                s_trace_frames = 2;
            s_trace_at = -1.0;
        }
    }
    if (s_stats) {
        static time_t last;
        static uint32_t frame0;
        time_t now = time(NULL);

        if (!last) { last = now; frame0 = s_frame; }
        if (now - last >= 5) {
            uint32_t nf = s_frame - frame0 ? s_frame - frame0 : 1;
            fprintf(stderr, "[GL] %.1f fps | por frame: %lu draws -> %lu lotes, %lu tris | "
                            "%lu decodes em %lds | %d texturas, %d superficies\n",
                    (double)(s_frame - frame0) / (double)(now - last),
                    s_n_draw / nf, s_n_flush / nf, s_n_tris / nf,
                    s_n_decode, (long)(now - last), s_tex_count, s_nsurf);
            last = now; frame0 = s_frame;
            s_n_draw = s_n_tris = s_n_decode = s_n_rtt = s_n_flush = 0;
        }
    }
}

static void module_env(void)
{
    const char *c = getenv("DW4_GL_CULL");

    if (c)
        s_cull_mode = strcmp(c, "inv") == 0 ? 2 : (c[0] == '0' ? 0 : 1);
    s_stats = getenv("DW4_GL_STATS") != NULL;
    ro_init("GL");
    if (getenv("DW4_GL_DBG"))
        s_dbg = atoi(getenv("DW4_GL_DBG"));
    if (getenv("DW4_GL_TRACE_AT"))
        s_trace_at = atof(getenv("DW4_GL_TRACE_AT"));
    if (getenv("DW4_GL_F9_AT")) {           /* o mesmo que apertar F9 naquele instante */
        s_trace_at = atof(getenv("DW4_GL_F9_AT"));
        s_trace_is_f9 = 1;
    }
}

/* O que o host (gl_host.c) chama. Mude DW4_GL_MODULE_ABI se a struct mudar. */
static const Dw4GlModule s_module = {
    .abi = DW4_GL_MODULE_ABI, .name = "OpenGL", .needs_gl = 1,
    .clear = be_clear, .draw = be_draw, .flip = be_flip, .present_sync = be_present_sync
};

/* Roda dentro do dlopen, na thread que carrega o modulo (a NV2A num reload).
 * A API corrente do EGL e por thread; o host a seleciona antes de criar o
 * contexto novo, mas os executaveis de 2026-11-24 00:14-00:16 nao faziam isso
 * e todo reload falhava com EGL_BAD_ATTRIBUTE. Faze-lo aqui conserta o reload
 * de uma sessao ja aberta com um host daqueles, sem reiniciar o jogo. */
__attribute__((constructor))
static void module_loaded(void)
{
    eglBindAPI(EGL_OPENGL_API);
}

__attribute__((visibility("default")))
const Dw4GlModule *dw4_gl_module(void)
{
    return &s_module;
}
