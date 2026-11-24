# CLAUDE.md — Digimon World 4 Recomp

Port nativo de **Digimon World 4 (Xbox, NTSC-U, title ID 42410002)** por recompilação estática com
o toolkit [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (v0.18.0, submódulo em
`xboxrecomp/`). Não é emulador: o `default.xbe` vira C em `src/recompiled/` e roda sobre o runtime
do toolkit, com renderizador próprio. Responda em português.

**Objetivo: rodar em Linux, macOS e Windows.** Hoje só o Linux funciona; os outros dois ainda
não foram começados. O renderizador que funciona é o OpenGL 3.3; um **back end Vulkan está em
desenvolvimento** e é o caminho para as três plataformas (macOS via MoltenVK).

## Estado (atualize ao mudar)

- **Jogável:** boot, vídeos, menus, criação do Digimon, save/load, lobby, tutorial e primeira fase,
  a 60 fps (CachyOS, NVIDIA). Fases seguintes, chefes e multijogador **não foram testados**.
- **Publicado:** release v0.1.0 (AppImage + tar.gz) em
  `github.com/lorransouzaaguiar/Digimon-World-4-Recomp` (privado; vai ser público e open source).
- **Em andamento:** back end Vulkan (`src/hooks/nv2a_vk_backend.c`, `src/hooks/shaders/`,
  `third_party/Vulkan-Headers`). Confira no `STATUS.md` e no código até onde ele chegou antes de
  assumir que funciona.
- **Aberto:** crash intermitente ao pular o vídeo de abertura com START (assinatura no
  `STATUS.md`); texgen não implementado; sem resolução interna maior nem widescreen; licença do
  projeto ainda não escolhida.

## Onde ler antes de mexer

| Arquivo | O que tem |
|---|---|
| `STATUS.md` | o que funciona, o que falta, bugs abertos |
| `PORT_NOTES.md` | mapa do guest e endereços, hooks, patches no toolkit (§4), renderização (§7), depuração e testes (§8), como cada bug foi achado |
| `docs/REFERENCIAS.md` | estudo de outros ports com o mesmo toolkit (o que reaproveitar, licenças, como publicam) |
| `README.md` / `README.pt-BR.md` | visão para jogador e contribuidor (en e pt-BR) |

Antes de escrever algo grande do zero, veja se o toolkit ou `docs/REFERENCIAS.md` já resolve.
Clones de referência, se precisar, vão em `refs/` com `--depth 1` (ignorado pelo git).

## Onde fica cada coisa

```
src/main.c                    host POSIX: sinais, crash report, log, boot do guest
src/hooks/                    todo o código próprio do runtime
  nv2a_gl_backend.c           renderizador OpenGL  -> build/libdw4_gl.so (recarrega a quente)
  nv2a_vk_backend.c, shaders/ renderizador Vulkan (em desenvolvimento)
  gl_host.c                   contexto EGL, carga/recarga do renderizador (fica no executável)
  recomp_manual.c             defaults de ambiente (RECOMP_*), overrides, init dos hooks
  d3d_fence.c  input_script.c  screenshot.c  stall_watch.c
src/recompiled/               C GERADO do default.xbe (ignorado; scripts/regen.sh)
launcher/                     launcher Qt Quick: dw4_launcher.py, qml/Main.qml, xiso.py
packaging/                    AppRun, .desktop, ícone, README do pacote
scripts/                      setup, regen, soak, triage, golden, f9_diff, package_release...
tests/scenarios/              roteiros de entrada (sintaxe em src/hooks/input_script.c)
patches/xboxrecomp-dw4.patch  todas as mudanças no toolkit
config/                       sementes de funções para o recompilador
game_data/ analysis/ save_data/ build*/ dist/ tools/ refs/ .venv/   locais, ignorados
```

## Comandos

```bash
export PATH=$PWD/.venv/bin:$PATH                 # ninja e capstone ficam no .venv
cmake --build build -j$(nproc)                   # tudo
cmake --build build --target dw4_gl              # só o renderizador (jogo aberto recarrega sozinho)
scripts/soak.py --golden                         # teste sem janela + frames de referência (~95 s)
scripts/soak.py --scenario <arquivo> --runs N    # outro roteiro / repetir
scripts/triage.py <log>                          # resumo e veredito de um log
scripts/regen.sh                                 # regenera src/recompiled/ (após mudar sementes)
./launcher.sh --selftest                         # launcher fora da tela
scripts/package_release.sh vX.Y.Z                # AppImage + tar.gz em dist/
```

## Regras

1. **Nada do jogo no git, nunca:** `game_data/`, `src/recompiled/`, `analysis/`, saves, capturas de
   referência. Hashes e endereços podem; bytes não. Confira `git status` antes de commitar.
2. **Não abrir janela do jogo nem do launcher sem avisar o usuário.** Teste com
   `scripts/soak.py` (sem janela) e `./launcher.sh --selftest` / `--screenshot`. Se só o usuário
   pode confirmar algo (imagem, som, controle), diga isso em vez de afirmar que funciona.
3. **Código portável entre Linux, macOS e Windows.** Código novo usa SDL2, Vulkan e C padrão;
   o que for específico de um sistema (sinais POSIX, `/proc`, `dlopen`, EGL, tratamento de falha
   de MMIO, caminhos XDG) fica isolado em arquivo ou bloco próprio, para ganhar um equivalente
   nas outras plataformas. Nada de DirectX. Hoje `CMakeLists.txt` recusa Windows e macOS, e
   `main.c`, `gl_host.c`, `stall_watch.c` e o empacotamento são só Linux: isso é dívida conhecida,
   não regra.
3a. **Dois renderizadores, um shader:** o fragment shader Vulkan é porte do OpenGL. Uma correção
   de imagem feita em um tem de ser repetida no outro. O OpenGL continua sendo o padrão e a
   referência até o Vulkan passar nos mesmos frames de referência; o de CPU (`DW4_SW_RENDER=1`)
   segue como árbitro.
4. **Código próprio em `src/hooks/`.** Não edite `src/recompiled/` à mão (é regenerado): corrija
   com override em `recomp_manual.c`, semente em `config/` ou patch no toolkit.
5. **Mudou algo em `xboxrecomp/`?** Atualize o patch
   (`git -C xboxrecomp diff > patches/xboxrecomp-dw4.patch`) e descreva em `PORT_NOTES.md` §4.
6. **Reiniciar ou não:** mudança em `nv2a_gl_backend.c` chega ao jogo aberto por recarga a
   quente; qualquer outra (executável, toolkit) exige reiniciar o jogo. Diga qual é o caso.
7. **Dois idiomas:** textos do launcher (`STR` em `dw4_launcher.py`), READMEs e notas de release
   sempre em pt-BR e en-US.
8. **Pacotes:** nenhum caminho da máquina de build e nenhuma biblioteca acima da glibc 2.38
   (`package_release.sh` verifica). Dados do usuário ficam em `~/.local/share/dw4-recomp/`.
9. **Commits e push só quando o usuário pedir.** Release publicado é ação externa: confirme antes.
10. **`RECOMP_PB_WALK=dma` e desligar `RECOMP_FLIP_PACING` quebram o jogo** (crash / corrupção de
    heap); não use nem exponha no launcher.

## Ao terminar uma tarefa

- Rode `scripts/soak.py --golden`; se a imagem mudou de propósito, confira e atualize as
  referências (`scripts/golden.py <shots> --update`).
- Atualize `STATUS.md`; em `PORT_NOTES.md` registre causa de bug, patch ou endereço novo.
- Diga o que foi verificado e o que não foi.

## Procedimentos (skills em `.claude/skills/`)

- `bug-de-imagem` — algo aparece errado, invisível ou com cor trocada.
- `crash-ou-travamento` — o jogo fecha, congela ou não avança.
- `criar-release` — empacotar, escrever as notas bilíngues e publicar uma versão.
