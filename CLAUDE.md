# CLAUDE.md — Guia de Arquitetura e Regras: Digimon World 4 (Port Nativo Linux)

## 1. Diretórios e Estrutura Obrigatória
Mantenha **100% dos arquivos, clones, scripts, referências e builds** estritamente dentro de `/home/lorran/Projetos/digimon-world-4-recomp/`:
- **Raiz do Projeto:** `/home/lorran/Projetos/digimon-world-4-recomp/`
- **Toolkit Base (`sp00nznet/xboxrecomp`):** `/home/lorran/Projetos/digimon-world-4-recomp/xboxrecomp/`
- **Arquivos do Jogo (`default.xbe` + assets):** `/home/lorran/Projetos/digimon-world-4-recomp/game_data/`
- **Código C Recompilado:** `/home/lorran/Projetos/digimon-world-4-recomp/src/recompiled/`
- **Hooks, Overrides e Backends Customizados:** `/home/lorran/Projetos/digimon-world-4-recomp/src/hooks/`
- **Repositórios de Referência (Opcional/Temporário):** `/home/lorran/Projetos/digimon-world-4-recomp/refs/`
- **Diretório de Build (CMake + Ninja):** `/home/lorran/Projetos/digimon-world-4-recomp/build/`
- **Logs de Progresso e Notas:** `STATUS.md` e `PORT_NOTES.md` na raiz.

## 2. Repositórios de Referência no Linux (`xboxrecomp`)
Antes de escrever código complexo do zero (especialmente o backend OpenGL 3.3, cache de texturas NV2A, shaders, áudio ou mapeamento de controles no Linux), consulte como outros ports nativos de Linux resolveram o mesmo problema usando o `xboxrecomp`:
- **Def Jam: Fight for NY (Linux / Steam Deck, xboxrecomp v0.16):** `https://github.com/vyanhursky/DefJamFFNY-recomp`
- **Amped 2 (Linux principal, xboxrecomp v0.14):** `https://github.com/bertibertig/Amped2-Recomp`
- **SSX 3 (Linux, xboxrecomp v0.13–v0.14):** `https://github.com/bertibertig/SSX3-Recomp` e `https://github.com/koenhendriks/SSX3-recomp`
- **BLiNX 2 (macOS / Linux, xboxrecomp v0.14):** `https://github.com/apexJCL/blinx2-recomp`

**O estudo desses repositórios já foi feito e está em `docs/REFERENCIAS.md`** (o que cada um faz, o que têm em comum, correções de runtime candidatas a portar, licenças). Leia-o antes de reestudar os clones ou de escrever algo grande do zero.

*Nota:* Se precisar inspecionar arquivos desses repositórios, clone-os com `--depth 1` dentro de `/home/lorran/Projetos/digimon-world-4-recomp/refs/` para manter tudo isolado no diretório do projeto.

## 3. Regras Técnicas para CachyOS (Arch Linux)
1. **Aceleração por GPU Obrigatória (OpenGL 3.3):** O rasterizador por software em CPU limita o lobby a ~2–3 FPS. Implemente o backend acelerado por GPU em `src/hooks/` via `nv2a_backend.h` (reaproveitando `d3d8_gl.c` / código do `xboxrecomp` e padrões dos repositórios de referência acima).
   - Implemente **cache de texturas** indexado por endereço físico da memória do Xbox + hash para evitar re-upload (`glTexImage2D`) e *unswizzling* em CPU a cada frame.
   - Mantenha um fallback para o rasterizador CPU via variável de ambiente (`DW4_SW_RENDER=1`) para depuração visual.
2. **Backend 100% Nativo Linux:** Use apenas SDL2, OpenGL 3.3, threads POSIX e áudio nativo. Nunca use Win32 API, DirectX 12 ou Media Foundation.
3. **Sistema de Arquivos FATX (Case-Insensitive):** Redirecione `D:\` para `/home/lorran/Projetos/digimon-world-4-recomp/game_data/` convertendo `\` para `/` e resolvendo caminhos sem distinção de maiúsculas/minúsculas.
4. **Ciclo de Build Iterativo:** Compile sempre com:
   `cmake --build /home/lorran/Projetos/digimon-world-4-recomp/build -j$(nproc)`
5. **Isolamento de Edições:** Mantenha hooks, backends e stubs em `src/hooks/`. Documente qualquer alteração manual em `src/recompiled/` com o endereço virtual (`0x00XXXXXX`) em `PORT_NOTES.md`.

## 4. Status Atual e Próximo Objetivo
- **Concluído:** Setup do `xboxrecomp`, recompilação estática do `default.xbe`, build nativo Linux e boot até o lobby in-game.
- **Foco Atual:** Substituir a rasterização em CPU pelo backend OpenGL 3.3 em `src/hooks/` com cache de texturas para atingir 60 FPS no lobby e liberar os testes de gameplay.