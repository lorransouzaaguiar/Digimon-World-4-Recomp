# Estudo dos ports de referência (xboxrecomp)

Feito em 2026-11-23 lendo os clones em `refs/` (só leitura; nada deles foi compilado ou executado).
Objetivo: reaproveitar o que já existe em vez de reinventar. **Leia este arquivo antes de escrever
qualquer coisa grande em renderização, threads, áudio, entrada ou ferramentas de teste.**

Os patches deles são contra versões antigas do toolkit (v0.13–v0.16); o nosso é **v0.18.0**. Antes
de portar qualquer um, confira se o upstream já não o incorporou.

## 1. Os ports

| Clone em `refs/` | Jogo | Toolkit | Linux | Renderização | Licença |
|---|---|---|---|---|---|
| `bertibertig_SSX3-Recomp` | SSX 3 | v0.14 + 1 patch | nativo | Vulkan via `nv2a_backend.h` (`src/render/vk_backend.c`) | GPL-3 |
| `bertibertig_Amped2-Recomp` | Amped 2 | v0.14 + 1 patch (3.868 linhas) | nativo | idem (mais novo; contém tudo do SSX 3 dele) | GPL-3 |
| `koenhendriks_SSX3-recomp` | SSX 3 | v0.13.1 + 1 patch (10.530 linhas) | nativo | Vulkan via `nv2a_backend.h` (`src/vk/`) | GPL-3 |
| `vyanhursky_DefJamFFNY-recomp` | Def Jam FFNY | fork v0.16 (submódulo **vazio** no clone) | nativo + Steam Deck | Vulkan (`d3d8_vk.c` no fork) | MIT |
| `apexJCL_blinx2-recomp` | BLiNX 2 | fork v0.14 (clone externo, **não está** no repo) | só via Proton | D3D11 (DXVK) / Metal; Linux nativo é só plano | MIT |

Nosso projeto: mesmo desenho dos três nativos (executor de pushbuffer + back end de GPU próprio),
com OpenGL 3.3 em `src/hooks/nv2a_gl_backend.c`.

**Licença importa:** copiar código dos três repositórios GPL-3 torna o nosso projeto GPL-3 se for
distribuído. Ideias, formatos e correções reescritas não têm esse efeito. O toolkit é MIT; Def Jam
e BLiNX 2 também, mas o código de runtime deles não está nos clones (só documentação).

## 2. O que eles têm em comum

### Para jogar
- **Limite de quadros no vblank** (todos): flip preso a 60 Hz. Nós: `RECOMP_FLIP_PACING=1`.
- **Back end de GPU com o rasterizador em CPU como referência** (todos): variável de ambiente
  para voltar à CPU e comparar frames. Nós: `DW4_SW_RENDER=1`.
- **Escala de resolução interna** (SSX 3 ×2, Amped 2, Def Jam, BLiNX 2): renderizar a N× 640×480.
  Temos desde 2026-11-24 (`DW4_RES`, `src/hooks/render_opts.h`).
- **Widescreen** (SSX 3 ×2, Amped 2, Def Jam): Amped 2 só comprime o x de todo vértice numa
  superfície mais larga (sem patch no jogo); os SSX 3 também alargam a projeção com um hook na
  câmera (corrige o culling nas laterais). Temos o do Amped 2 (`DW4_ASPECT`); o hook de câmera
  fica para quando o culling incomodar.
- **Cache de shaders/pipelines em disco** (Def Jam; SSX 3 koenhendriks compila em thread). Não
  precisamos ainda (um shader só).
- **Mapa de teclas configurável** (`keys.ini`, SSX 3 koenhendriks) e tratamento do controle
  duplicado do Steam Deck (Def Jam). Não temos.
- **Diretório de saves/dados por variável de ambiente** (todos). Nós: `RECOMP_SAVE_DIR`, `DW4_GAME_DIR`.

### Para desenvolver
- **Crash handler** com registradores do guest, retornos na pilha do guest e últimos alvos de
  chamada indireta (todos). Temos; Def Jam também captura `SIGABRT` (adotado em 2026-11-23).
- **Despejo de threads por sinal** (`SIGUSR1`; SSX 3 koenhendriks, BLiNX 2). Temos
  (`scripts/dump_threads.sh`).
- **Watchdog/amostrador de travamento**: `RECOMP_WATCHDOG_SECS` (todos) e "sem flip por N ms →
  pilha da thread do jogo no log" (`AMPED2_STALL_MS`, `RECOMP_SLOW_STACKS`). Temos os dois
  (`DW4_STALL_MS`, `src/hooks/stall_watch.c`).
- **Entrada por roteiro** (todos): `RECOMP_PAD_SCRIPT`/`RECOMP_PAD_LIVE` do próprio toolkit;
  Def Jam ancora os passos em linhas do log em vez de tempo; SSX 3 koenhendriks navega os menus
  lendo o estado do jogo (`src/autonav.c`). Nós: `DW4_INPUT_SCRIPT` ancorado em flips
  (`f<N>:tecla`), cenários em `tests/scenarios/`.
- **Execução sem janela** (`SDL_VIDEODRIVER=dummy`, `gamescope --backend headless`) e laço de
  "soak" que classifica cada execução em ok / travou / crash / saiu (BLiNX 2 `scripts/soak.py`,
  SSX 3 `scripts/race_loop.sh`). Temos: `scripts/soak.py` (`RECOMP_HEADLESS=1`).
- **Frames de referência ("goldens")** comparados a cada mudança (Def Jam, BLiNX 2). Temos:
  `scripts/golden.py`, `tests/golden/`.
- **Semear funções a partir do log** (`Failed to resolve` → seeds → regen; todos). Temos
  (`tools.seed_from_log`, `scripts/regen.sh`).
- **Triagem de log** numa tela (`scripts/triage.py`, Def Jam). Temos: `scripts/triage.py`.
- **Log com buffer / assíncrono** (Def Jam D31, BLiNX 2, SSX 3 koenhendriks): log sem buffer
  custava até 33% da thread do executor e travadas de 100–300 ms. Adotado (buffer de 64 KB).
- **Patches do toolkit num arquivo só, reaplicado por script** (bertibertig, koenhendriks). Temos
  (`patches/xboxrecomp-dw4.patch`).

## 3. Correções de runtime que eles precisaram (candidatas a portar)

| Tema | Quem | Onde ler | Nosso estado |
|---|---|---|---|
| Trava de DPC: DPCs excluídos enquanto alguma thread está em DISPATCH_LEVEL (DSOUND corrompia vozes) | Amped 2 | `patches/xboxrecomp/README.md:110-116`, patch `:750-821` | **não temos** — 1ª candidata |
| ISRs de APU/OHCI/vblank concorrendo com DPCs corrompiam listas do DSOUND | BLiNX 2 | `openspec/changes/archive/2026-10-07-irq-safe-points/` | não temos (toolkit tem `RECOMP_DISPATCH_GATE`) |
| Threads do guest uma por vez | BLiNX 2, Def Jam (D28), SSX 3 koenhendriks | `archive/2026-10-17-guest-one-cpu/`; `PROGRESS.md:83` | existe no v0.18: `RECOMP_GUEST_LOCK=1`, desligado |
| "Fence hold": o jogo sobrescrevia o pushbuffer antes de o executor rodar | Def Jam | `src/recomp_manual.c:1932-1988` | parcial (`src/hooks/d3d_fence.c`) |
| Resgate de espera de fence já satisfeita, com log | SSX 3 koenhendriks | `src/ssx3_overrides.c:2259-2456` | parcial (sem log) |
| Vblank fica pendente até o título dar ack; 60 Hz exatos | Amped 2 | README `:59-65`, `:95` | conferir no v0.18 |
| Cache de vértices pós-transformação + programas pré-decodificados (23 → 55–60 fps) | SSX 3 ×2, Amped 2 | README `:54-58`, patch `:2356-2416` | não temos |
| Textura obsoleta em estágio com programa NONE; pitch só para formatos lineares | Amped 2 | README `:46`, `:66` | conferir (pode explicar o bug visual da seção 7.1 do `PORT_NOTES.md`) |
| `NV_MAX_INDICES` baixo truncava draws em silêncio | BLiNX 2 | `archive/2026-10-07-pb-index-batch-cap/` | v0.18 tem 65536 |
| Heap: quarentena e guarda de double-free | BLiNX 2 | `archive/2026-11-03-heap-quarantine/` | não temos |
| Página física com dois donos (imagem × janela contígua) | BLiNX 2 | `archive/2026-11-03-contig-image-alias/` | resolvido com `RECOMP_CONTIG_AVOID_IMAGE=1` |
| Funções do guest com nome de função da libc + `-rdynamic` derrubavam o processo | Amped 2 | README `:73-75` | conferido: não ocorre aqui |

## 4. Corrupção de heap (`malloc(): unaligned tcache chunk detected`)

- Aqui: aparecia em ~15 s com o jogo sem limite de quadros (~2000 fps); sumiu com
  `RECOMP_FLIP_PACING=1`. Não reproduziu sob gdb nem em 3 execuções com AddressSanitizer
  (`build-asan/`, `-DDW4_ASAN=ON`). **Causa não localizada.**
- `koenhendriks_SSX3-recomp` tem o mesmo aborto (~3 em 35 boots), na thread SDL, em
  `SDL_CreateTexture`; suspeita deles: redimensionamento do alvo de render concorrendo com a
  thread de flip (`CLAUDE.md:1005-1010`). Também não resolveram.
- Hipóteses a testar, na ordem: trava de DPC (Amped 2) · `RECOMP_GUEST_LOCK=1` · "fence hold"
  do Def Jam · corrida entre `xbox_FramebufferWindowPresent` e a thread da janela.

- **2026-11-24: uma causa confirmada aqui** — plugar um controle com o jogo aberto. O
  `sdl2-compat` libera e refaz uma lista global de joysticks em `SDL_NumJoysticks()` e na tradução
  do evento de conexão, esta sem o lock; duas threads fazendo isso = double free. Corrigido abrindo
  controles só na thread que bombeia eventos (`PORT_NOTES.md` §4.9). Os ports de referência
  (`host_pad.c`, `ssx3_pad.c`) reescaneiam a cada 250 ms a partir da thread do USB: janela menor,
  mesma exposição. O aborto do koenhendriks "na thread SDL" pode ser da mesma família.

## 5. Lições registradas por eles

- O código do guest assume **uma CPU**; cada ponto extra de preempção é uma corrida em potencial.
- Nunca bloquear segurando o lock da CPU do guest ou de um dispositivo.
- "Um acerto é evidência, uma ausência não"; ler totais, não linhas impressas; conferir em que
  espaço de endereços um número está (Def Jam, `docs/agent/diagnostics.md`).
- Draws que "vêm e vão" sob carga = estouro de orçamento por frame, não erro de pushbuffer (Amped 2).
- `clock()` no POSIX é tempo de CPU: leituras de fps ficam erradas (SSX 3).
- `perf --user-callchains` funciona em processo filho; anexar não, com `ptrace_scope=1` (Amped 2).

## 6. OpenGL × Vulkan

Usamos OpenGL 3.3 porque era a regra do `CLAUDE.md` e bastou: 60 fps no lobby com ~5.000 draws por
frame depois do agrupamento de draws. Os back ends Vulkan existentes são GPL-3 e dependem dos
patches de runtime de cada autor (toolkit v0.13/v0.14), então adotá-los não é "copiar um arquivo":
é portar o patch deles para o v0.18, trazer Vulkan-Headers/volk/glslc e aceitar a GPL-3.
Reavaliar se: (a) as fases ficarem abaixo de 60 fps por custo de draw, ou (b) quisermos
escala de resolução/AA/quadros em voo já prontos e a GPL-3 for aceitável.

**Atualização (2026-11-24):** temos um back end Vulkan próprio (`src/hooks/nv2a_vk_backend.c`,
`PORT_NOTES.md` §7.5), escolhido no launcher ao lado do OpenGL. Das referências vieram as ideias
(Vulkan-Headers fixado, `glslc -mfmt=c`, UBO de combiners por lote); nenhum código GPL-3 foi
copiado, e o desenho difere em dois pontos: sem swapchain (leitura de volta, como o OpenGL) e
Vulkan 1.0 com cache de pipelines em vez de 1.3 + EDS3.

## 7. Como eles publicam releases (estudado em 2026-11-24)

| Port | O que vai no release | Como é montado |
|---|---|---|
| Amped 2, SSX 3 (bertibertig) | **binário pronto** + `run.sh` + `README.txt` + `game_files/` vazio + licenças; `.tar.gz` (Linux), `.zip` (Windows), `SHA256SUMS` | `scripts/package_release.sh <versão>`, build em contêiner Ubuntu 24.04 (glibc 2.39); notas em `docs/release-notes/vX.Y.Z.md`; SSX 3 inclui `install_game.sh` que lê a ISO |
| SSX 3 (koenhendriks) | binário + launcher + `keys.example.ini` + README, `.zip` Windows | `tools/package.ps1` |
| Def Jam | **nenhum binário do jogo**: um instalador (AppImage/exe) que compila na máquina do jogador a partir do dump dele; regra explícita "never distribute built game executables (they embed lifted code)" | GitHub Actions por tag → rascunho de release com checksums e proveniência |
| BLiNX 2 | idem: só um "wizard" que clona o projeto e compila do dump; o pacote resultante é privado | `packaging/`, `release-wizard.yml` |

Nenhum distribui dados do disco. Eles se dividem em **publicar o executável** (três ports) ou
**publicar só a ferramenta que compila** (dois ports). Nós: `scripts/package_release.sh` no primeiro
modelo; sem contêiner (não há docker/podman na máquina), então a glibc mínima é a do CachyOS do dia.
