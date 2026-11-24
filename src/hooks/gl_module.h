/**
 * Interface entre o host (gl_host.c, dentro do executavel) e o renderizador
 * recarregavel (nv2a_gl_backend.c -> libdw4_gl.so).
 *
 * Todas as chamadas vem da thread NV2A, com o contexto OpenGL corrente.
 * O modulo nao guarda nada que precise sobreviver a um reload: no reload o
 * host destroi o contexto (e com ele todos os objetos GL), cria outro e
 * carrega a copia nova da biblioteca, com as variaveis estaticas zeradas.
 */
#ifndef DW4_GL_MODULE_H
#define DW4_GL_MODULE_H

#include "nv2a_backend.h"

#define DW4_GL_MODULE_ABI 1

typedef struct {
    int abi;
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    void (*present_sync)(void);
} Dw4GlModule;

/* Exportado por libdw4_gl.so. */
const Dw4GlModule *dw4_gl_module(void);

#endif /* DW4_GL_MODULE_H */
