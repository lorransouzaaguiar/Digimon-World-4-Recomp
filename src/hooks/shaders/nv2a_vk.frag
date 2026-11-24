#version 450
/* Porte de nv2a_rc_eval (nv2a_combiner.c) e rc_stage_fetch (nv2a_pb_exec.c),
 * identico ao fragment shader do back end OpenGL (nv2a_gl_backend.c): so
 * mudam as declaracoes (bloco de uniformes, set/binding, locations). Uma
 * correcao feita num deve ser repetida no outro. */
#include "nv2a_ubo.glsl"

layout(set = 0, binding = 1) uniform sampler2D u_tex0;
layout(set = 0, binding = 2) uniform sampler2D u_tex1;
layout(set = 0, binding = 3) uniform sampler2D u_tex2;
layout(set = 0, binding = 4) uniform sampler2D u_tex3;

layout(location = 0) in vec4 v_col;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_t0;
layout(location = 3) in vec4 v_t1;
layout(location = 4) in vec4 v_t2;
layout(location = 5) in vec4 v_t3;
layout(location = 6) in vec4 v_spec;
layout(location = 7) in float v_fog;

layout(location = 0) out vec4 o;

vec4 R[16];
bool g_kill = false;

float map_in(float x, uint b)
{
    float p = max(x, 0.0);
    switch ((b >> 5u) & 7u) {
    case 0u: return p;
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * p - 1.0;
    case 3u: return -2.0 * p + 1.0;
    case 4u: return p - 0.5;
    case 5u: return -p + 0.5;
    case 6u: return x;
    default: return -x;
    }
}

float map_out(float x, uint m)
{
    switch (m) {
    case 0x08u: return x - 0.5;
    case 0x10u: return x * 2.0;
    case 0x18u: return (x - 0.5) * 2.0;
    case 0x20u: return x * 4.0;
    case 0x30u: return x * 0.5;
    default:    return x;
    }
}

vec3 mo3(vec3 v, uint m)
{
    return clamp(vec3(map_out(v.x, m), map_out(v.y, m), map_out(v.z, m)), -1.0, 1.0);
}

vec3 rd_rgb(uint b)
{
    vec4 r = R[b & 0xFu];
    vec3 v = ((b & 0x10u) != 0u) ? vec3(r.a) : r.rgb;
    return vec3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));
}

float rd_a(uint b)
{
    vec4 r = R[b & 0xFu];
    return map_in(((b & 0x10u) != 0u) ? r.a : r.b, b);
}

vec4 stage(int st, vec4 c, sampler2D s)
{
    uint mode = (u.ctl.y >> uint(st * 5)) & 0x1Fu;
    float q = (c.w != 0.0) ? c.w : 1.0;

    if (mode == 0u) return vec4(0.0, 0.0, 0.0, 1.0);
    if (mode == 1u || mode == 2u) return texture(s, c.xy / q * u.tscale[st].xy);
    if (mode == 3u) return texture(s, c.xy * u.tscale[st].xy);
    if (mode == 4u) return clamp(c, 0.0, 1.0);
    if (mode == 5u) {
        for (int j = 0; j < 4; j++) {
            bool ge = ((u.ctl.z >> uint(st * 4 + j)) & 1u) != 0u;
            if (ge ? (c[j] >= 0.0) : (c[j] < 0.0)) g_kill = true;
        }
    }
    return vec4(0.0);
}

vec4 rc_eval(vec4 t0, vec4 t1, vec4 t2, vec4 t3)
{
    uint n = min(u.ctl.x & 0xFFu, 8u), flags = u.ctl.x >> 8u;

    for (int i = 0; i < 16; i++) R[i] = vec4(0.0);
    R[3] = vec4(u.fogc.rgb, v_fog); R[4] = v_col; R[5] = v_spec;
    R[8] = t0; R[9] = t1; R[10] = t2; R[11] = t3;
    R[12].a = ((u.ctl.y & 0x1Fu) != 0u) ? t0.a : 1.0;
    for (uint s = 0u; s < n; s++) {
        uint icw = u.cw[s].x, aicw = u.cw[s].y, ocw = u.cw[s].z, aocw = u.cw[s].w;
        uint fl = ocw >> 12u, afl = aocw >> 12u;
        R[1] = u.f0[((flags & 0x010u) != 0u) ? s : 0u];
        R[2] = u.f1[((flags & 0x100u) != 0u) ? s : 0u];
        bool mux_cd = ((flags & 1u) != 0u) ? (R[12].a >= 0.5)
                                           : ((int(R[12].a * 255.0) & 1) != 0);
        vec3 A = rd_rgb(icw >> 24u), B = rd_rgb(icw >> 16u);
        vec3 C = rd_rgb(icw >> 8u),  D = rd_rgb(icw);
        float aA = rd_a(aicw >> 24u), aB = rd_a(aicw >> 16u);
        float aC = rd_a(aicw >> 8u),  aD = rd_a(aicw);
        vec3 pab = ((fl & 2u) != 0u) ? vec3(dot(A, B)) : A * B;
        vec3 pcd = ((fl & 1u) != 0u) ? vec3(dot(C, D)) : C * D;
        vec3 sum = ((fl & 4u) != 0u) ? (mux_cd ? pcd : pab) : pab + pcd;
        vec3 ab = mo3(pab, fl & 0x38u), cd = mo3(pcd, fl & 0x38u), ms = mo3(sum, fl & 0x38u);
        float apab = aA * aB, apcd = aC * aD;
        float asum = ((afl & 4u) != 0u) ? (mux_cd ? apcd : apab) : apab + apcd;
        float aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);
        float acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);
        float ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);
        uint d;
        d = (ocw >> 4u) & 0xFu;
        if (d != 0u) { R[d].rgb = ab; if ((fl & 0x80u) != 0u) R[d].a = ab.b; }
        d = ocw & 0xFu;
        if (d != 0u) { R[d].rgb = cd; if ((fl & 0x40u) != 0u) R[d].a = cd.b; }
        d = (ocw >> 8u) & 0xFu;  if (d != 0u) R[d].rgb = ms;
        d = (aocw >> 4u) & 0xFu; if (d != 0u) R[d].a = aab;
        d = aocw & 0xFu;         if (d != 0u) R[d].a = acd;
        d = (aocw >> 8u) & 0xFu; if (d != 0u) R[d].a = ams;
        R[0] = vec4(0.0);
    }
    vec4 res;
    if (u.fin.x != 0u || u.fin.y != 0u) {
        uint f0 = u.fin.x, f1 = u.fin.y, ff = f1 & 0xFFu;
        R[1] = u.fc0; R[2] = u.fc1;
        vec3 a = ((ff & 0x40u) != 0u) ? 1.0 - R[5].rgb : R[5].rgb;
        vec3 b = ((ff & 0x20u) != 0u) ? 1.0 - R[12].rgb : R[12].rgb;
        R[14] = vec4(((ff & 0x80u) != 0u) ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);
        vec3 E = rd_rgb(f1 >> 24u), F = rd_rgb(f1 >> 16u);
        R[15] = vec4(E * F, 0.0);
        vec3 A = rd_rgb(f0 >> 24u), B = rd_rgb(f0 >> 16u);
        vec3 C = rd_rgb(f0 >> 8u),  D = rd_rgb(f0);
        res = vec4(D + C * (1.0 - A) + B * A, rd_a(f1 >> 8u));
    } else {
        res = R[12];
    }
    return clamp(res, 0.0, 1.0);
}

void main()
{
    vec4 c;

    if (u.alpha.z == 0) {
        c = v_col * texture(u_tex0, v_uv);
    } else {
        vec4 t0 = stage(0, v_t0, u_tex0), t1 = stage(1, v_t1, u_tex1);
        vec4 t2 = stage(2, v_t2, u_tex2), t3 = stage(3, v_t3, u_tex3);
        if (g_kill) discard;
        c = rc_eval(t0, t1, t2, t3);
    }
    if (u.alpha.x != 0) {
        int a = int(c.a * 255.0 + 0.5), r = u.alpha.y;
        bool p;
        switch (u.alpha.x) {
        case 0x200: p = false;  break;
        case 0x201: p = a <  r; break;
        case 0x202: p = a == r; break;
        case 0x203: p = a <= r; break;
        case 0x204: p = a >  r; break;
        case 0x205: p = a != r; break;
        case 0x206: p = a >= r; break;
        default:    p = true;
        }
        if (!p) discard;
    }
    o = c;
}
