#version 450
/* Back end Vulkan: mesmo vertex shader do OpenGL (nv2a_gl_backend.c), com as
 * convencoes do Vulkan: z do NDC em [0,1] e y para baixo (a linha 0 da imagem
 * e a linha 0 da superficie do guest, sem espelhamento). */
#include "nv2a_ubo.glsl"

layout(location = 0) in vec4 a_pos;      /* x, y, z em pixels; rhw */
layout(location = 1) in vec4 a_col;
layout(location = 2) in vec2 a_uv;       /* texels (caminho simples) */
layout(location = 3) in vec4 a_t0;       /* s,t,r,q por estagio */
layout(location = 4) in vec4 a_t1;
layout(location = 5) in vec4 a_t2;
layout(location = 6) in vec4 a_t3;
layout(location = 7) in vec4 a_spec;
layout(location = 8) in float a_fog;

layout(location = 0) out vec4 v_col;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_t0;
layout(location = 3) out vec4 v_t1;
layout(location = 4) out vec4 v_t2;
layout(location = 5) out vec4 v_t3;
layout(location = 6) out vec4 v_spec;
layout(location = 7) out float v_fog;

void main()
{
    float w = (a_pos.w > 0.0) ? 1.0 / a_pos.w : 1.0;
    vec2 xy = a_pos.xy / u.surf.xy * 2.0 - 1.0;
    float z = a_pos.z / u.surf.z;

    gl_Position = vec4(xy * w, z * w, w);
    v_col = a_col;
    v_uv = a_uv / u.texsize.xy;
    v_t0 = a_t0; v_t1 = a_t1; v_t2 = a_t2; v_t3 = a_t3;
    v_spec = a_spec;
    v_fog = a_fog;
}
