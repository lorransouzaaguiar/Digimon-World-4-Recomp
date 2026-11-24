/* Bloco de uniformes por lote (std140). Espelha a struct Ubo de
 * nv2a_vk_backend.c -- mude os dois juntos. */
layout(set = 0, binding = 0, std140) uniform Batch {
    vec4  surf;        /* largura, altura, zmax, - */
    vec4  texsize;     /* caminho simples: largura, altura da textura */
    ivec4 alpha;       /* funcao (0 = sem teste), ref, usa combiners, - */
    vec4  tscale[4];   /* xy por estagio */
    vec4  fogc;
    uvec4 cw[8];       /* color_icw, alpha_icw, color_ocw, alpha_ocw */
    vec4  f0[8];
    vec4  f1[8];
    vec4  fc0;
    vec4  fc1;
    uvec4 fin;         /* final0, final1 */
    uvec4 ctl;         /* control, stage_program, clip_plane_mode */
} u;
