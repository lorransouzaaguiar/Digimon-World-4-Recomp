# CLAUDE.md — Digimon World 4 Recomp

Port nativo de **Digimon World 4 (Xbox, NTSC-U, title ID 42410002)** por recompilação estática com
o toolkit [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp) (v0.18.0, submódulo em
`xboxrecomp/`). Não é emulador: o `default.xbe` vira C em `src/recompiled/` e roda sobre o runtime
do toolkit, com renderizador próprio. Responda em português.

**Objetivo: rodar em Linux, macOS e Windows.** Linux e Windows funcionam (Windows desde a v0.4.0,
por compilação cruzada); o macOS ainda não foi começado. Há **dois renderizadores, OpenGL 3.3 e Vulkan**, e o jogador escolhe entre
eles no launcher (Gráficos → API gráfica). O Vulkan (1.0, sem extensões) é o padrão desde 2026-11-24, com o OpenGL de reserva; ele é o
caminho para as três plataformas (macOS via MoltenVK).

## Estado (atualize ao mudar)

- **Jogável:** boot, vídeos, menus, criação do Digimon, save/load, lobby, tutorial e primeira fase,
  a 60 fps (CachyOS, NVIDIA). Fases seguintes, chefes e multijogador **não foram testados**.
  Em 2026-11-26 o usuário relatou Linux e Windows 11 jogáveis, fluidos e no mesmo patamar de
  desempenho (excelente, mas só na máquina dele; no Windows real, 60 fps em 4K com Vulkan e
  quedas para ~40 fps com OpenGL nas áreas pesadas), e que acredita que já dá para zerar
  (expectativa dele; ninguém jogou até o fim).
- **Publicado:** release v1.0.0 (2026-11-26; número escolhido pelo usuário por não haver mais bug conhecido de
  imagem e som, mesmo sem o jogo ter sido zerado; antes v0.5.2, de 2026-11-25; AppImage + tar.gz + `.zip` de Windows com launcher;
  todo release leva Linux e Windows, e o Windows não é mais apresentado como "experimental" — pedido
  do usuário; antes v0.5.1, v0.5.0, v0.4.1, v0.4.0, v0.3.2, v0.3.1, v0.3.0, v0.2.0 e v0.1.0) em
  `github.com/lorransouzaaguiar/Digimon-World-4-Recomp` (preparado para ficar público: licença
  MIT, histórico com e-mail noreply; a visibilidade quem muda é o usuário).
- **Vulkan (branch `DW4R-1`):** `src/hooks/nv2a_vk_backend.c` passa nos mesmos frames de
  referência do OpenGL sem janela, até o lobby, a 60 fps, e o usuário jogou com janela em
  2026-11-24 sem problemas. Não testado fora de NVIDIA — veja `STATUS.md` antes de afirmar mais que isso.
- **Desempenho (branch `DW4R-2`, 2026-11-24):** esperas ocupadas removidas (195% → 28% de um
  núcleo no lobby); Vulkan apresenta por swapchain com quadros em voo quando há janela (modo
  leitura sem janela); modo paralelo jogo/executor opcional (`RECOMP_KICK_AHEAD=1`). Confirmado
  pelo usuário jogando em 4K e 1440p a 60 fps nas áreas pesadas (RTX 5070); não testado em
  outras placas nem em máquinas fracas.
- **Widescreen (DW4R-2):** só 16:9 e 21:9 (16:10 retirada). Fade, HUD e minimapa tratados em
  `src/hooks/render_opts.h` (`ro_squeeze`, chamada no flush do lote); recorte do guest (scissor)
  nos dois renderizadores. Detalhes e casos em `PORT_NOTES.md` §7.6.
- **Interface em widescreen (DW4R-4, v0.3.2):** além da caixa de diálogo abaixo, o rodapé do menu
  do START é cortado na coluna 4:3 e a linha do "Loading..." do boot e a faixa do título vão de
  ponta a ponta; `DW4_WS_DIALOG=0` desliga tudo isso.
- **Caixa de diálogo (DW4R-4, 2026-11-24):** em 16:9 e 21:9 o fundo e as linhas vão de ponta a ponta;
  retrato, nome e texto ficam na coluna 4:3 do meio — o usuário viu ancorado à esquerda e
  preferiu centralizado (`ro_squeeze`; `DW4_WS_DIALOG=0` desliga). 16:9: conferido sem
  janela no lobby e pelo usuário jogando. 21:9: só pelo usuário jogando ("tá legal"), nenhum teste
  sem janela. Lojas, escolhas e 2–4 jogadores não foram vistos.
- **Windows (`DW4R-5`, mesclada na `main` em 2026-11-25): prévia experimental, no release `v0.4.0`.**
  Compilação cruzada com MinGW-w64 (`scripts/build_win.sh` → `build-win/dw4_recomp.exe`), Vulkan e
  OpenGL dentro do `.exe`, SDL2 para janela/controle/áudio. Passa em `soak.py --wine --golden` e
  `--wine --gamescope --opengl --golden`; o usuário jogou no Wine com janela (Vulkan até 1440p
  16:9; OpenGL por pouco tempo) e em **Windows 11 real com a v0.4.0** ("funcionou", sem logs).
  **Launcher no Windows (`DW4R-6`, v0.4.1):** `DigimonWorld4-Recomp.exe` +
  Python embutível + PySide6 no pacote, dados em `%APPDATA%\dw4-recomp\`; testado só no Wine.
  Teclado e Proton não foram testados. Veja `STATUS.md` (Port para Windows).
- **Atalhos, mostrador e atualização (`DW4R-6`, na v0.5.0, 2026-11-25):** fechar o jogo por
  atalho (`Ctrl`+`Q`; `Back`+`Start` segurados 1 s), mostrador de FPS/CPU/GPU (`F2`; `L3`+`R3`) e
  teclas do teclado-controle trocáveis, 4 portas de controle (`RECOMP_USB_PADS=4`; multijogador
  nunca testado) e atualização pelos releases do GitHub (AppImage e `.zip` de Windows se atualizam; `.tar.gz` e
  código-fonte abrem a página) e escolha do monitor (Gráficos → Janela; confirmada pelo usuário
  no Linux e no Wine). A v0.5.0 é a primeira que se atualiza sozinha. Tudo configurável no launcher. Testado sem janela, no Wine e com
  um servidor local no lugar do GitHub; o usuário jogou no Linux em 2026-11-25 e relatou tudo
  funcionando. **Não testado: mais de um controle.** (Windows real e a atualização entre dois
  releases foram confirmados depois pelo usuário, veja abaixo e o `STATUS.md`.)
- **Áudio repetindo (relato na v0.5.0, só Linux):** corrigido na v0.5.1 (`DW4R-7`; evento de
  vblank perdido na camada POSIX do toolkit, `PORT_NOTES.md` §4.20); medido sem janela e
  confirmado pelo usuário jogando com o AppImage. `DW4_CRI_TRACE=1` diagnostica.
- **Atualização no Windows (relato na v0.5.0 → v0.5.1, Windows 11 real):** falhou por caminho acima
  de 260 caracteres na pasta de preparo; corrigido na `DW4R-8` (pacote sem os arquivos longos +
  caminhos `\\?\` no `updater.py`) junto com o ícone da janela no Windows, na **v0.5.2**. O ícone
  foi visto pelo usuário no Windows 11; **a atualização v0.5.1 → v0.5.2 foi feita por ele em Windows 11 real e funcionou**
  (a do AppImage no Linux só rodou contra o servidor de teste). `package_win.sh` recusa caminho acima de 125 caracteres.
- **Aberto:** fechar pela janela caiu na saída dentro do gamescope (`PORT_NOTES.md` §8); texgen só nos modos lineares (sphere/reflection/normal map faltam);
  HUD em widescreen só conferido com 1 jogador.
- **Chão que escurecia / sombras (2026-11-26, `DW4R-12`, na v1.0.0):** a causa era o texgen
  ignorado no passe de sombra projetada; corrigido em `ffp_vertex()` no toolkit (`PORT_NOTES.md`
  §4.8 e §7.8). O usuário jogou no Linux com Vulkan e com OpenGL (1440p 16:9; Central Area e
  cavernas): o chão não escurece mais e os Digimon têm sombra correta ("tá 100%"). Sem janela:
  goldens passam nos dois renderizadores e no Wine (referências atualizadas, com sombra).
  Em Windows 11 real o usuário jogou o pacote de teste da `DW4R-12` em 2026-11-26 ("tá perfeito"; Vulkan, 1440p).
- **Crash ao pular a abertura (2026-11-26, `DW4R-12`, na v1.0.0):** corrida entre a thread
  principal e a mwidle do CRI (no console a prioridade impede); a passada da mwidle agora respeita
  a seção crítica do CRI (`sub_00257FB0` embrulhada em `recomp_manual.c`, `PORT_NOTES.md` §4.21;
  `DW4_CRI_IDLE_GATE=0` desliga). Sem janela: 128 de 128 limpas (antes ~5%), Wine 16 de 16. O usuário
  rodou em Windows 11 real em 2026-11-26 sem crash (2 execuções; não dá para saber pelo log se pulou a abertura).
- **Memória do guest:** `RECOMP_HEAP_RECLAIM=1` é default desde a v0.3.1 (sem ela a memória
  contígua enche e a tela fica branca após alguns minutos); `DW4_NO_HEAP_RECLAIM=1` desliga.
- **Licença: MIT** (`LICENSE`, escolhida em 2026-11-24). Testado só no começo do jogo: Terminal
  Area, Central Area, Death Valley e Humid Cave.

## Onde ler antes de mexer

| Arquivo | O que tem |
|---|---|
| `STATUS.md` | o que funciona, o que falta, bugs abertos |
| `PORT_NOTES.md` | mapa do guest e endereços, hooks, patches no toolkit (§4), renderização (§7), depuração e testes (§8), como cada bug foi achado |
| `docs/WIDESCREEN_DIALOGO.md` | caixa de diálogo de ponta a ponta em widescreen (feito na `DW4R-4`, publicado na v0.3.2): mapa das peças, regra, o que falta ver |
| `docs/REFERENCIAS.md` | estudo de outros ports com o mesmo toolkit (o que reaproveitar, licenças, como publicam) |
| `README.md` / `README.pt-BR.md` | visão para jogador e contribuidor (en e pt-BR) |

## Não reinventar a roda: os ports de referência

Quatro jogos já foram portados com o mesmo toolkit. **Antes de implementar qualquer coisa não
trivial** (renderizador, shader, áudio, entrada, pacing, correção de runtime, launcher,
empacotamento, port para outro sistema, ferramenta de teste), veja como eles resolveram:

| Jogo | Repositório | Clone local |
|---|---|---|
| Amped 2 | `github.com/bertibertig/Amped2-Recomp` | `refs/bertibertig_Amped2-Recomp` |
| SSX 3 | `github.com/bertibertig/SSX3-Recomp` e `github.com/koenhendriks/SSX3-recomp` | `refs/bertibertig_SSX3-Recomp`, `refs/koenhendriks_SSX3-recomp` |
| Def Jam: Fight for NY | `github.com/vyanhursky/DefJamFFNY-recomp` | `refs/vyanhursky_DefJamFFNY-recomp` |
| BLiNX 2 | `github.com/apexJCL/blinx2-recomp` | `refs/apexJCL_blinx2-recomp` |

1. Leia primeiro `docs/REFERENCIAS.md` (estudo já feito: o que cada um tem, correções de runtime
   candidatas, licenças). Depois abra os clones: `grep` em `refs/` pelo tema e os documentos de
   desenho deles (`docs/vulkan-backend.md`, `docs/VULKAN.html`, READMEs, `PROGRESS.md`).
2. Falta um clone ou está velho? `git clone --depth 1 <url> refs/<dono>_<repo>` (ignorado pelo git).
3. **Licença:** Amped 2 e os dois SSX 3 são **GPL-3** — aproveite ideias, formatos e correções
   reescritas, **nunca copie código** (este projeto é MIT, incompatível com receber código GPL). Def Jam,
   BLiNX 2 e o toolkit são MIT.
4. Ao terminar, diga ao usuário o que veio das referências e onde o nosso desenho difere e por
   quê; o que aprender de novo sobre eles vai para `docs/REFERENCIAS.md`.
5. Vale também para o toolkit (`xboxrecomp/`): confira se ele já tem a função ou a variável
   `RECOMP_*` antes de escrever um hook.

## Onde fica cada coisa

```
src/main.c                    host: log, boot do guest, parte comum do relatório de crash
src/hooks/                    todo o código próprio do runtime
  nv2a_gl_backend.c           renderizador OpenGL  -> build/libdw4_gl.so (recarrega a quente)
  nv2a_vk_backend.c           renderizador Vulkan  -> build/libdw4_vk.so (recarrega a quente)
  shaders/                    GLSL do Vulkan (glslc -> SPIR-V embutido); porte do shader do OpenGL
  gl_module.h                 interface comum aos dois renderizadores (ABI)
  render_opts.h               resolução interna, proporção e regras de widescreen (fade, HUD, minimapa), comum aos dois
  widescreen.c                alarga o culling do jogo em widescreen (gancho sub_000BC050_enter)
  gl_host.c                   escolhe a API (DW4_RENDERER), contexto EGL, carga/recarga (no executável)
  host_os.h                   o que depende do sistema (caminhos, ambiente, crash, despejo de threads):
                              host_os_posix.c (Linux) e host_os_win32.c (Windows)
  recomp_manual.c             defaults de ambiente (RECOMP_*), overrides, init dos hooks
  d3d_fence.c  input_script.c  screenshot.c  stall_watch.c
  quit_hotkey.c               atalhos de teclado/controle: fechar o jogo e ligar/desligar o mostrador
  osd.c                       mostrador de FPS/CPU/GPU: um lote 2D entregue ao renderizador em uso (gl_host.c)
  prof.c                      amostrador de CPU (DW4_PROF; relatorio: scripts/prof_report.py)
src/recompiled/               C GERADO do default.xbe (ignorado; scripts/regen.sh)
launcher/                     launcher Qt Quick (Linux e Windows): dw4_launcher.py, qml/Main.qml, xiso.py,
                              updater.py (atualizacao pelos releases do GitHub; sem Qt)
packaging/                    AppRun, .desktop, ícone, README do pacote; windows/ (entrada .exe e README)
scripts/                      setup, regen, soak, triage, golden, f9_diff, package_release...
tests/scenarios/              roteiros de entrada (sintaxe em src/hooks/input_script.c)
patches/xboxrecomp-dw4.patch  todas as mudanças no toolkit
third_party/Vulkan-Headers    submódulo fixado (só cabeçalhos; dispensa o SDK do Vulkan)
config/                       sementes de funções para o recompilador
game_data/ analysis/ save_data/ build*/ dist/ tools/ refs/ .venv/   locais, ignorados
```

## Comandos

```bash
export PATH=$PWD/.venv/bin:$PATH                 # ninja e capstone ficam no .venv
cmake --build build -j$(nproc)                   # tudo
cmake --build build --target dw4_gl              # só o renderizador OpenGL (jogo aberto recarrega sozinho)
cmake --build build --target dw4_vk              # só o renderizador Vulkan (idem)
scripts/soak.py --golden                         # teste sem janela + frames de referência (~95 s), Vulkan (padrão)
scripts/soak.py --opengl --golden                # o mesmo com o renderizador OpenGL
DW4_RENDERER=opengl ./run.sh                     # escolhe a API sem o launcher: vulkan | opengl | cpu
DW4_RES=1440 DW4_ASPECT=16:9 ./run.sh            # resolução interna e proporção (vale em soak.py com -e)
scripts/soak.py --scenario <arquivo> --runs N    # outro roteiro / repetir
scripts/soak.py --gamescope --golden             # com janela dentro de um gamescope sem tela (swapchain)
scripts/soak.py -e DW4_PROF=/tmp/p.txt -e DW4_PROF_FROM=3900 && scripts/prof_report.py /tmp/p.txt   # perfil de CPU
scripts/triage.py <log>                          # resumo e veredito de um log
scripts/regen.sh                                 # regenera src/recompiled/ (após mudar sementes)
scripts/test_pad.sh                              # plugar/desplugar controle virtual (sem jogo, ~5 s)
scripts/test_updater.py                          # atualizacao do launcher contra um servidor local (~2 s); --online consulta o GitHub
scripts/soak.py --scenario tests/scenarios/osd.txt --timeout 60            # mostrador de desempenho (capturas osd_*.bmp)
scripts/soak.py --scenario tests/scenarios/quit_teclado.txt --timeout 60   # atalho de fechar: da INCOMPLETO de proposito; vale o [QUIT] no log
./launcher.sh --selftest                         # launcher fora da tela
./launcher.sh --screenshot 1 docs/launcher_graphics.png --lang en --size 1120x1000 --defaults   # imagens dos READMEs
scripts/package_release.sh vX.Y.Z                # AppImage + tar.gz em dist/
scripts/build_win.sh                             # compilação cruzada para Windows (MinGW-w64) -> build-win/; log em build-win/build.log
scripts/soak.py --wine --golden                  # o .exe de Windows no Wine, sem tela, com os mesmos frames de referência (Vulkan)
scripts/soak.py --wine --gamescope --opengl --golden   # o OpenGL do .exe (precisa de uma tela: gamescope sem monitor)
scripts/package_win.sh vX.Y.Z                    # .zip de Windows em dist/ (+ .debug.exe com símbolos, não publicar); rode DEPOIS do package_release.sh
./launcher.sh --wine                             # launcher do Linux iniciando o .exe de Windows no Wine (ABRE JANELA; saves em build-win/save_data)
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
   nas outras plataformas: use `src/hooks/host_os.h`. Nada de DirectX. Hoje `CMakeLists.txt`
   recusa macOS e só aceita Windows por MinGW; a recarga a quente (`dlopen`), `prof.c`, o despejo
   de threads e o launcher empacotado são só Linux: isso é dívida conhecida, não regra.
3a. **Dois renderizadores, um shader, os dois ficam:** o jogador escolhe a API no launcher; nunca
   remova uma para implementar a outra. O fragment shader Vulkan é porte do OpenGL: uma correção
   de imagem (estado, shader, textura) feita em um tem de ser repetida no outro e testada com
   `soak.py --golden` **e** `soak.py --opengl --golden`. O Vulkan é o padrão (jogado pelo usuário
   em 2026-11-24); o de CPU (`DW4_SW_RENDER=1`) segue como árbitro.
   O Vulkan fica no 1.0 sem extensões (MoltenVK, drivers antigos); a única exceção são as de
   janela (WSI/swapchain), opcionais: sem elas ele apresenta por leitura de volta.
4. **Código próprio em `src/hooks/`.** Não edite `src/recompiled/` à mão (é regenerado): corrija
   com override em `recomp_manual.c`, semente em `config/` ou patch no toolkit. Gancho de entrada
   (`void sub_XXXXXXXX_enter(void)` em `recomp_manual.c`) só vale depois de
   `scripts/regen.sh --lift` + build completo.
5. **Mudou algo em `xboxrecomp/`?** Atualize o patch
   (`git -C xboxrecomp diff > patches/xboxrecomp-dw4.patch`) e descreva em `PORT_NOTES.md` §4.
6. **Reiniciar ou não:** mudança em `nv2a_gl_backend.c` / `nv2a_vk_backend.c` (e `shaders/`) chega
   ao jogo aberto por recarga a quente (só no Linux; no Windows os renderizadores ficam no `.exe`); qualquer outra (executável, toolkit) exige reiniciar o jogo. Diga qual é o caso.
7. **Dois idiomas:** textos do launcher (`STR` em `dw4_launcher.py`), READMEs e notas de release
   sempre em pt-BR e en-US.
8. **Pacotes:** nenhum caminho da máquina de build e nenhuma biblioteca acima da glibc 2.38
   (`package_release.sh` verifica). Dados do usuário ficam em `~/.local/share/dw4-recomp/` (Linux)
   e `%APPDATA%\dw4-recomp\` (Windows).
8a. **`dist/` só guarda a versão mais nova.** Ao criar uma versão, os pacotes das outras saem de
   `dist/` (`scripts/dist_prune.sh vX.Y.Z`, que `package_release.sh` e `package_win.sh` já chamam).
   Vai junto o `.debug.exe` de Windows das versões antigas; os pacotes publicados continuam no GitHub.
8b. **Título do release: só a versão, `vX.Y.Z`.** Nada mais: nem o nome do projeto, nem descrição
   do que mudou. O que mudou vai nas notas.
9. **Commits e push só quando o usuário pedir.** Release publicado é ação externa: confirme antes.
10. **`RECOMP_PB_WALK=dma` e desligar `RECOMP_FLIP_PACING` quebram o jogo** (crash / corrupção de
    heap); não use nem exponha no launcher.

## Ao terminar uma tarefa

- Rode `scripts/soak.py --golden` (e `--opengl --golden` se mexeu em renderização ou em `osd.c`; `scripts/test_updater.py` se mexeu em `launcher/updater.py`; e
  `scripts/build_win.sh` + `scripts/soak.py --wine --golden` se mexeu em `host_os*`, `main.c`,
  `gl_host.c`, CMake ou toolkit); se a imagem mudou de propósito, confira e atualize as
  referências (`scripts/golden.py <shots> --update`).
- Atualize `STATUS.md`; em `PORT_NOTES.md` registre causa de bug, patch ou endereço novo.
- Diga o que foi verificado e o que não foi.

## Procedimentos (skills em `.claude/skills/`)

- `bug-de-imagem` — algo aparece errado, invisível ou com cor trocada.
- `crash-ou-travamento` — o jogo fecha, congela ou não avança.
- `criar-release` — empacotar, escrever as notas bilíngues e publicar uma versão.
