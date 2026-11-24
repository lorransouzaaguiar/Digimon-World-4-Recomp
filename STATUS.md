# STATUS — Digimon World 4 (Xbox NTSC-U) → Linux nativo

Última atualização: 2026-11-23

| Marco | Estado |
|---|---|
| **M1** Ambiente & Análise | ✅ concluído |
| **M2** Recompilação Estática | ✅ concluído |
| **M3** Build Nativo Limpo | ✅ concluído |
| **M4** Boot & Tela Título | ✅ concluído (com ressalva: vídeo por rasterizador em CPU, não `d3d8_gl.c`) |

## M1 — Ambiente & Análise ✅

- Pacotes: `cmake 4.5.1`, `clang/llvm 23.1.5`, `python 3.14`, `sdl2-compat`, `libepoxy`, `mesa`, `glu`,
  `openal`, `ffmpeg`, `openssl`, `gdb` — todos presentes.
- **Faltando no sistema:** `ninja` → `sudo pacman -S --needed ninja`.
  Contornado sem sudo: `.venv/` do projeto tem `ninja` e `capstone` (o pipeline Python exige capstone,
  que também não está no sistema). Use `export PATH=$PWD/.venv/bin:$PATH` antes do cmake.
- Toolkit: `./xboxrecomp` @ `37b61d5` (v0.18.0), libs estáticas compiladas em `./xboxrecomp/build`.
- JSON de análise: `analysis/Default_analysis.json` (fora de `game_data/`, senão o jogo o vê em `D:\\`).
- XBE: o arquivo no disco chama-se `game_data/Default.xbe` (D maiúsculo) — o host procura sem
  distinguir caixa.

### Análise do XBE (`analysis/Default_analysis.json`)

| Campo | Valor |
|---|---|
| Título / Title ID | `DigimonWorld4` / `0x42410002`, Retail, NTSC-U |
| Build | 2005-02-18, debug path `c:\Alchemy\sources\apps\insight\XboxRelease\NewDigimonWorld.exe` (engine **Alchemy**) |
| Base / Entry point | `0x00010000` / `0x000121A4` |
| Kernel thunks | `0x00309BC0`, **128 imports** |
| TLS | index `0x003B55C8`, zero-fill 144 bytes |
| XDK | 1.0.5849 — XAPILIB, LIBCMT, LIBCPMT, **D3D8, D3DX8, XGRAPHC, DSOUND linkados estaticamente** |
| Rede | nenhuma lib XONLINE/XNET no XBE (não há Xbox Live para stubar) |
| Middleware | CRI Sofdec (seções `PSFD_I/B/P/00`) para FMV; `DOLBY` |

Seções de código: `.text` (2,6 MB), `PSFD_*`, `D3D`, `D3DX`, `XGRPH`, `DSOUND`, `XPP`, `DOLBY`.
Dados: `.rdata` `0x00309BC0`, `.data` `0x0038B4A0` (1,4 MB virtual, 172 KB inicializados).

Assets: `area.afs` (489 MB), `ndmw.afs` (660 MB), `ndmw.igb` — tudo na raiz de `D:\`.

## M2 — Recompilação Estática ✅

Comandos (rodados de dentro de `./xboxrecomp`, com `P=../.venv/bin/python`):

```bash
$P -m tools.xbe_parser   ../game_data/Default.xbe --json ../analysis/Default_analysis.json
$P -m tools.disasm       ../game_data/Default.xbe --analysis-json ../analysis/Default_analysis.json -v -o ../analysis/disasm        # todas as seções de código
$P -m tools.func_id      ../game_data/Default.xbe --functions ../analysis/disasm/functions.json \
      --strings ../analysis/disasm/strings.json --xrefs ../analysis/disasm/xrefs.json -o ../analysis/func_id -v
$P -m tools.abi_analysis ../game_data/Default.xbe --disasm-dir ../analysis/disasm \
      --func-id-dir ../analysis/func_id --output-dir ../analysis/abi -v
$P -m tools.recomp       ../game_data/Default.xbe --game-name "Digimon World 4" --all --split 250 \
      --gen-dir ../src/recompiled --disasm-dir ../analysis/disasm --func-id-dir ../analysis/func_id \
      --abi-dir ../analysis/abi -o ../analysis/recomp
```

Resultado inicial: **23.047 funções** (hoje **23.472**, com as sementes de `config/` — rode `scripts/regen.sh`), 0 falhas, 2,25 M linhas de C em 94 chunks (`src/recompiled/`),
2.912 `thiscall`, 7 alvos não resolvidos (stubs em `recomp_stubs_unresolved.c`), 470 instruções sem
tradução (`RECOMP_UNIMPL`; quase todas são dados desmontados como código em `DSOUND` — `outsb/insd/arpl` —
mais `pfmul/pfadd` 3DNow! num caminho alternativo de CPU).

## M3 — Build Nativo Limpo ✅

```bash
export PATH=$PWD/.venv/bin:$PATH      # só enquanto o ninja do sistema não estiver instalado
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build -j$(nproc)        # ~16 s em 16 threads, gera build/dw4_recomp
```

- `CMakeLists.txt`: toolkit via `add_subdirectory`, código gerado como object library (`-O1 -fno-strict-aliasing -fwrapv`),
  `-rdynamic` para o crash handler resolver `sub_XXXXXXXX`.
- `src/main.c`: host POSIX (o template do toolkit é Win32) — handler `sigaction` → `xbox_PosixMmioFault`,
  relatório de crash com registradores/pilha do guest, `D:\` → `game_data/`, saves → `save_data/`.
- `src/hooks/recomp_manual.c`: `recomp_lookup_manual`, logs de ICALL, `recomp_unimpl`, defaults de ambiente.
- Nenhum erro de compilação/link; nenhum arquivo gerado foi editado.

## M4 — Boot & Tela Título ✅

```bash
./run.sh                               # RECOMP_KEYBOARD=1 por padrão; D:\ = ./game_data
DW4_SHOT=build/shots/s ./run.sh        # grava BMPs do que está na tela
```

Verificado em 2026-11-23 (captura em `docs/title_screen.png`):

1. tela **"Loading…"**
2. logos **Sofdec / ADX**
3. **FMV de abertura** (CRI Sofdec recompilado — sem stub de vídeo)
4. **tela título** "DIGIMON WORLD 4 — > Press START button <"
5. loop de demonstração (FMVs) alternando com a tela título, estável por ~3 min sem crash,
   sem `[ICALL] Failed`, sem `[UNIMPL]`, sem erro de decodificação de MMIO.

Kernel 128/128 imports resolvidos; `D:\` montado em `game_data/` (busca sem distinção de caixa);
NV2A via executor de pushbuffer; APU emulada com saída SDL2.

Bloqueios resolvidos no caminho (detalhes em `PORT_NOTES.md`):
funções só alcançadas por ponteiro (281 sementes) · alocador do Alchemy (`RECOMP_EXT_VMA`) ·
semáforo da GPU (`src/hooks/d3d_fence.c`) · KEVENT do vblank (`RECOMP_TITLE_KEVENTS`) ·
caminhos de arquivo no heap alto (patch no toolkit) · DirectSound/AC'97 (`RECOMP_AC97_READY`
+ 2 patches no toolkit).

### Entrada e menus ✅ (2026-11-23)

START pula o FMV → tela título → menu **Entry** (Controller 1–4 / Player 1) → tela de
**nome do Digimon** com teclado virtual. Verificado com roteiro (`DW4_INPUT_SCRIPT`) e com
controle físico (Spark N5 em modo Xbox, lido via SDL). Folha de capturas: `docs/menus_sheet.png`.

Três coisas faltavam para existir controle na porta 0 (detalhes em `PORT_NOTES.md` §3.7):
`xbox_OhciInit()` nunca era chamado · `RECOMP_USB=1` · `RECOMP_CONTIG_AVOID_IMAGE=1`.

Teste automatizado: `scripts/play.sh 75 "22:enter 30:enter 36:enter" 3` → `build/shots/sheet.png`.

### Lobby (Terminal Area: Home Server) ✅ (2026-11-23)

Criação do Digimon → "press START to log into the Digital World" → o lobby 3D carrega, a câmera
de apresentação roda e o diálogo do Digi-Elf aparece. O congelamento que havia aqui era a espera
de fence do D3D (`PORT_NOTES.md` §3.8).

`./quickstart.sh` faz esse caminho sozinho (pula a abertura, START, cria o Digimon) e deixa o
jogo aberto no lobby.

### Back end OpenGL 3.3 ✅ (2026-11-23) — `src/hooks/nv2a_gl_backend.c`

O rasterizador em CPU fazia ~3 fps no lobby. Com o back end OpenGL (interface `nv2a_backend.h`,
como os ports Linux Amped 2 / SSX 3 fazem com Vulkan) o lobby roda a **~60 fps**, com ~4.800 draws
e ~31 mil triângulos por frame — medido ainda em **`llvmpipe`** (OpenGL por software), porque a
GPU estava indisponível (ver abaixo). Imagem conferida em `docs/lobby_gl_sheet.png`: logos, título,
menus, criação do Digimon, lobby com HUD e diálogos, Heal Circle e Commander Room, sem os polígonos
pretos do caminho em CPU.

- Seletor: `DW4_SW_RENDER=1` volta ao rasterizador em CPU. `DW4_GL_STATS=1` mostra fps/draws.
- Texturas paletizadas (P8) precisaram de um patch no toolkit (a interface não repassava a paleta;
  sem ela 90% da tela saía branca).
- Limite conhecido: só estágio de textura 0 × cor difusa (a interface não passa combiners nem os
  estágios 1–3); sem mipmaps; retângulo de clip/scissor não é repassado.

**Validado na GPU (2026-11-23, após reboot):** `[GL] back end OpenGL em EGL device 0 (NVIDIA):
NVIDIA GeForce RTX 5070 … 3.3.0 NVIDIA 620.73.04`. Menus e título a 60 fps; lobby entre **45 e
60 fps** (~4.800 draws / ~31 mil triângulos por frame). O limite agora é a CPU: o executor
decodifica e transforma tudo numa thread e o back end faz um `glBufferData` + troca de estado por
draw. Folha de capturas: `docs/lobby_gl_sheet.png`.

- `RECOMP_FLIP_PACING=1` virou default: sem ele, com a GPU, o jogo passava de 2000 fps e caía em
  segundos com `malloc(): unaligned tcache chunk detected`. **A corrida que corrompe o heap não foi
  encontrada, só deixou de ser provocada** (não reproduz sob gdb); se o crash voltar, é por aí.
- (Histórico) antes do reboot a NVIDIA estava inutilizável — driver 620.73 instalado com módulo
  620.70 carregado — e tudo caía no `llvmpipe`. `DW4_GL_ALLOW_SW=1` aceita `llvmpipe` se precisar.

### Ressalvas / não verificado

- **Vídeo não passa pelo `d3d8_gl.c`.** O jogo linka o D3D8 do XDK estaticamente e escreve direto
  no pushbuffer NV2A; o frame é rasterizado em **CPU** pelo executor do toolkit e apresentado numa
  janela SDL2 (renderer OpenGL). Um back end OpenGL 3.3 próprio (`nv2a_backend.h`) é o próximo passo.
- **Além do lobby/Commander Room nada foi verificado** (sair para uma fase, combate, save).
- **Artefatos nos menus 3D:** algumas capturas mostram polígonos pretos/faixas. Pode ser a captura
  lendo o buffer no meio do desenho ou erro real do rasterizador — falta conferir na janela.
- `cli`/`sti` em `0x00308809`/`0x0030881A` (seção XPP, driver USB) são alcançados e tratados como
  no-op; sem efeito visível até aqui.
- **Áudio não ouvido.** A APU inicializa e o DirectSound roda, mas não conferi a saída de som.
- **Desempenho não medido** além de "60 flips/s na tela título"; o FMV roda pelo rasterizador em CPU.
- 8 patches locais em `./xboxrecomp` (`patches/xboxrecomp-dw4.patch`) — um `git pull` no toolkit
  pode conflitar.

## Ferramentas de teste e diagnóstico ✅ (2026-11-23)

Feitas a partir do que os ports de referência têm (implementação própria; detalhes em
`PORT_NOTES.md` §8):

| Ferramenta | Onde | Estado |
|---|---|---|
| Rodar **sem janela** e classificar ok/crash/travou/incompleto | `scripts/soak.py` | validado (lobby em ~95 s, GPU ativa) |
| Roteiro de entrada ancorado em **frames do jogo** | `src/hooks/input_script.c`, `tests/scenarios/` | validado tela a tela |
| Frames de referência | `scripts/golden.py`, `tests/golden/` (4 frames) | validado (2 runs conferem) |
| Triagem de log numa tela | `scripts/triage.py` | validado |
| Amostrador de travamento | `src/hooks/stall_watch.c` (`DW4_STALL_MS`) | validado com limiar baixo |
| Crash handler com `SIGABRT`; log com buffer | `src/main.c` | compilado; `SIGABRT` ainda não exercitado |
| Build com AddressSanitizer | `-DDW4_ASAN=ON` → `build-asan/` | 3 runs sem detecção |

Não ficou bom: o passo `tecla!` (esperar a tela mudar) não é confiável com os fundos animados dos
menus; os cenários usam contagem de frames.

`./quickstart.sh` agora usa `tests/scenarios/quickstart.txt` (mesmo caminho validado, sem encerrar).
**A versão com janela do quickstart não foi rodada depois da troca para frames** — o caminho foi
validado só sem janela.

## Back end Vulkan ✅ (2026-11-24, branch `DW4R-1`) — `src/hooks/nv2a_vk_backend.c`

Segunda API gráfica, **ao lado** do OpenGL (que continua existindo e é o padrão). O jogador escolhe
no launcher: Gráficos → **API gráfica** → OpenGL 3.3 / Vulkan / CPU. Sem launcher:
`DW4_RENDERER=vulkan ./run.sh`. Se a API escolhida não sobe, o jogo abre com a outra (e, por último,
com a CPU). Desenho e diferenças para os ports de referência em `PORT_NOTES.md` §7.5.

- **Verificado sem janela** (RTX 5070): `scripts/soak.py --vulkan --golden` → os 4 frames de
  referência conferem com os do OpenGL; lobby a 60 fps (~4.000 draws → ~20 lotes, 10 pipelines);
  fallback Vulkan → OpenGL; recarga a quente (`cmake --build build --target dw4_vk`);
  OpenGL sem regressão (`scripts/soak.py --golden`); launcher `--selftest` e captura da tela Gráficos.
- **Confirmado pelo usuário (2026-11-24):** jogou com janela em Vulkan, "funcionando legal".
- **Não verificado:** além do que o usuário jogou nessa sessão; AMD/Intel; pacote de release com `libdw4_vk.so` (o script foi atualizado, não rodado);
  camadas de validação do Vulkan (não instaladas).
- Só no OpenGL: captura F9 com rastreio e sonda de pixel. Bug de imagem se investiga no OpenGL e a
  correção de shader é repetida em `shaders/nv2a_vk.frag`.
- Build: precisa do `glslc` (pacote `shaderc`) e do submódulo `third_party/Vulkan-Headers`
  (`git submodule update --init`); sem eles o CMake avisa e compila só o OpenGL.

## Recarga a quente do renderizador ✅ (2026-11-24)

`cmake --build build --target dw4_gl` com o jogo aberto troca o renderizador no próximo flip
(`[GL] renderizador RECARREGADO`), sem reiniciar e sem perder o ponto do jogo. Detalhes e limites
em `PORT_NOTES.md` §7.0. Validado sem janela (2 reloads durante o cenário do lobby).

## Bugs de imagem

- **Ponte invisível no fim do tutorial** e **transição cinza antes do "Press START"**: mesma causa —
  o executor não aplicava a matriz de textura da função fixa (patch 4.8 no toolkit). Transição do
  título validada sem janela; **ambas confirmadas pelo usuário jogando em 2026-11-24**
  (`PORT_NOTES.md` §7.3). Progresso verificado no jogo: tutorial completo até a ponte para o portão.

- **Modelos 3D salpicados de preto na seleção de Digimon**: giro das tiras de triângulos + sentido
  de face no renderizador (`PORT_NOTES.md` §7.4). Validado sem janela e confirmado pelo usuário.

## Plugar o controle com o jogo aberto fechava o jogo — RESOLVIDO (2026-11-24)

`malloc(): unaligned tcache chunk detected` ao conectar o controle. Corrida entre a thread da
janela e a do USB emulado na lista de joysticks do `sdl2-compat` (patch 4.9 no toolkit,
`PORT_NOTES.md` §4.9; achado pelo core dump). Reproduzido sem jogo com `scripts/test_pad.sh`
(código antigo: 8/8 abortam; corrigido: 3/3 passam). **Falta o usuário confirmar com o controle de
verdade.** Não tem relação com o Vulkan (aconteceu numa sessão em Vulkan, mas o defeito é da
camada de entrada). É candidato a ser também a "corrida que corrompe o heap" nunca localizada
(mesma mensagem) — **não confirmado**.

## Crash intermitente conhecido (aberto)

- **Pular a abertura com START durante o vídeo às vezes encerra o jogo** (2 de 8 execuções sem
  janela em 2026-11-24, tanto no build de desenvolvimento quanto no de release). Assinatura no log:
  `[ICALL] alvo 0x00000000 ... ret guest 0x00277028` (ou `0x0026A1E0`) logo após o START, depois
  `HalReturnToFirmware routine=2` (o próprio jogo desiste) e um SIGSEGV em `glBindFramebuffer` na
  saída. É o mesmo caso visto antes e não reproduzido (`ret guest 0x00277037`). Reproduzir:
  roteiro `f1000:enter` + `wait_flips:500`, várias vezes. Causa não investigada.

## Launcher e pacote de release

- **Launcher** (`./launcher.sh`, PySide6 + QML, `launcher/`): telas Jogar, Gráficos, Controles,
  Saves e dados, Suporte; pt-BR/en. Lê a imagem do disco direto (`launcher/xiso.py`) ou uma pasta.
  `--play`, `--selftest`, `--screenshot`.
- **Pacote** (`scripts/package_release.sh <versão>`): `dist/*.AppImage` (69 MB) + `.tar.gz` +
  `SHA256SUMS`. Traz launcher congelado (PyInstaller sobre Python portátil), jogo e bibliotecas
  (SDL2 da roda `pysdl2-dll`, libepoxy, libcrypto). glibc mínima 2.38, imposta pelo script.
  Dados gravados em `~/.local/share/dw4-recomp/`.
- **Conferido:** AppImage roda `--selftest`, renderiza as telas fora da tela e inicia o jogo sem
  janela até a tela título com as bibliotecas do pacote; importação de imagem testada com imagens
  **sintéticas** (partição e completa).
- **Não conferido:** imagem de disco real; a SDL2 embutida abrindo janela/áudio/controle de
  verdade; os diálogos de arquivo; qualquer outra distribuição.

## Estudo dos ports de referência

`docs/REFERENCIAS.md` (2026-11-23): comparação dos 5 clones em `refs/`, o que têm em comum para
jogar e para desenvolver, correções de runtime candidatas a portar e a discussão OpenGL × Vulkan.

## Direção: Vulkan e três plataformas (decidido em 2026-11-24)

- Alvo do projeto: **Linux, macOS e Windows**. Hoje só Linux funciona.
- **Back end Vulkan em desenvolvimento** (`src/hooks/nv2a_vk_backend.c`, `src/hooks/shaders/`,
  `third_party/Vulkan-Headers`); o OpenGL 3.3 segue como padrão e referência até o Vulkan passar
  nos mesmos frames de referência.
- Só Linux hoje, a portar: `CMakeLists.txt` (recusa Windows/macOS), `src/main.c` (sinais POSIX,
  falha de MMIO), `gl_host.c` (EGL, `dlopen`), `stall_watch.c` (`/proc`), empacotamento
  (AppImage), pasta de dados XDG do launcher. macOS em Apple Silicon precisa ainda de um decodificador
  de falhas de MMIO para aarch64 (o nosso é x86-64); BLiNX 2 e Def Jam já rodam em macOS com o
  mesmo toolkit (`docs/REFERENCIAS.md`).

## Próximos passos

1. Lobby a 60 fps estáveis: agrupar draws com o mesmo estado e evitar um `glBufferData` por draw.
1. Seguir do lobby até uma fase (novos alvos de ICALL → `tools.seed_from_log build/run.log …`
   + `scripts/regen.sh`).
3. Instalar `ninja` do sistema (`sudo pacman -S --needed ninja`) e, opcionalmente,
   `python-capstone`, para dispensar a `.venv`.

## Layout do projeto

| Caminho | Conteúdo |
|---|---|
| `xboxrecomp/` | toolkit (v0.18.0 + 8 patches locais) |
| `game_data/` | disco do jogo (`D:\`) |
| `src/recompiled/` | C gerado (não editar) |
| `src/hooks/`, `src/main.c` | host POSIX e hooks |
| `config/` | sementes de funções |
| `analysis/` | saídas do disasm/func_id/abi/recomp + logs |
| `scripts/` | `regen.sh`, `scan_code_pointers.py`, `gdb_threads.sh` |
| `patches/` | diff do toolkit |
| `save_data/` | saves/cache do guest |
| `build/` | build CMake + Ninja |
| `.venv/` | `capstone` + `ninja` (pip) |
