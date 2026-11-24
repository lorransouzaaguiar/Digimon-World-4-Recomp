# PORT_NOTES — Digimon World 4 (Xbox NTSC-U) → Linux nativo

Notas técnicas do port: endereços do guest, hooks, patches no toolkit e por quê.
Progresso por marco fica em `STATUS.md`.

## 1. Código gerado (`src/recompiled/`)

**Nenhum arquivo gerado foi editado à mão.** Tudo foi resolvido por sementes de
funções (`config/`), variáveis de ambiente do runtime, hooks em `src/hooks/` e
três correções no toolkit (seção 4). Para regenerar: `scripts/regen.sh`.

## 2. Mapa do guest (XDK 5849, engine Alchemy, CRI Sofdec/ADX)

| VA | O que é | Observação |
|---|---|---|
| `0x000121A4` | entry point (`xbe_entry_point`) | CRT → `PsCreateSystemThreadEx(0x00012E72, ctx=0x00012130)` |
| `0x00012130` | `main` do jogo | só alcançado por ponteiro (contexto da thread) |
| `0x00295303` | `_threadstartex` (CRT) | engolido por `sub_002952F9` (chamada sem retorno + `int3`); semeado |
| `0x0006F7F0` | sbrk do alocador Alchemy (dlmalloc) | varre o espaço de endereços com `NtQueryVirtualMemory` → precisa de `RECOMP_EXT_VMA` |
| `0x002BAD98` | `g_pDevice` do D3D8 (ponteiro) | device em `0x002BADA0` |
| device`+0x2C` / `+0x30` | fence submetido / ponteiro do semáforo da GPU | semáforo em `0x80001000` |
| device`+0x1DBC` | KEVENT do vblank, montado à mão | precisa de `RECOMP_TITLE_KEVENTS` |
| `0x002B1500` | `D3D_BlockOnTime` (espera de fence, laço em `0x002B1637`) | destravado por `src/hooks/d3d_fence.c` |
| `0x002ABBC0` | `BlockUntilVerticalBlank` | `KeWaitForSingleObject(device+0x1DBC)` |
| `0x0024D910`, `0x0024D990` | threads CRI (vsync / servidor) | esperam o vblank via `0x0024D660` |
| `0x0024D590` | thread CRI `mwidle` | faz o I/O dos AFS; se auto-suspende, a vsync a resume |
| `0x00195BE0` | laço de espera do loader Alchemy | `Sleep(16)` até o status CRI == 3 |
| `0x002E76A5`, `0x002EDEFF` | DirectSound (seção `DSOUND`) | acesso direto à APU `0xFE80xxxx` / AC'97 `0xFEC00000` |

Arquivos que o jogo abre: `D:\ndmw.igb`, `D:\ndmw.afs`, `D:\area.afs`; procura `D:\drivers.ini`
(não existe no disco, falha esperada). Saves: `\Device\Harddisk0\partition1\{T,U}DATA\42410002`
→ `save_data/`.

## 3. Bloqueios resolvidos (em ordem)

1. **Funções só alcançadas por ponteiro** (`[ICALL] Failed to resolve VA 0x00081270`, …).
   O Alchemy é quase todo vtable. `scripts/scan_code_pointers.py` varre todo dword alinhado e
   todo imediato (`xrefs.json`, `data_imm`) que aponte para código logo após padding/`ret`/`jmp`
   e que o desmontador leia como função → `config/seed_functions.json` (272 sementes).
   Alvos vistos em execução → `config/seed_observed.json` (9, via `tools.seed_from_log`).
   Total: 23.047 → 23.472 funções.
   - *Lição:* aceitar "alinhado em 16 + referenciado por tabela" sem exigir padding gerou
     sementes falsas (`0x00020000`, `0x00021000`, … são constantes, não ponteiros) que partem
     funções reais. Regra desativada; valores com os 12 bits baixos zerados são rejeitados.
   - O log de falha de ICALL mantém o texto em inglês `Failed to resolve VA` porque
     `tools.seed_from_log` casa exatamente essa frase.
2. **Laço infinito em `NtQueryVirtualMemory`** (ordinal 217, 400 M de chamadas): o sbrk do
   Alchemy procura uma região `MEM_FREE` grande; sem rastreador toda região livre tem 4 KB.
   → `RECOMP_EXT_VMA=1` (191 MB em `0x74000000..0x7FFE0000`). Efeito colateral bom: com a base
   reservada em bloco, a janela contígua `0x80000000` passou a mapear.
3. **Thread principal presa em `D3D_BlockOnTime`**: o executor de pushbuffer não sabia onde
   escrever o semáforo (`BACK_END_WRITE_SEMAPHORE_RELEASE`, método `0x1D70`).
   → `src/hooks/d3d_fence.c`: thread do host observa `MEM32(0x002BAD98)+0x30` e chama
   `nv2a_pb_set_semaphore_target()`.
4. **Threads CRI girando a ~100 mil voltas/s**: o XBE não importa `KeInitializeEvent`; o KEVENT
   do vblank é montado à mão. → `RECOMP_TITLE_KEVENTS=1`.
5. **`[FILE] ? … FAILED`**: nomes de arquivo alocados no heap do Alchemy (acima de `0x74000000`)
   eram rejeitados por `bridge_va_mapped()`. → patch no toolkit (4.1). Foi isto que destravou a
   leitura dos AFS.
6. **Travamento logo após "Loading…"** em DirectSound. → `RECOMP_AC97_READY=1` + patches 4.2 e 4.3.

7. **Nenhum controle na porta 0** (nem gamepad, nem `RECOMP_KEYBOARD`). Três causas em cadeia:
   - `xbox_OhciInit()` não é chamado por ninguém no toolkit (nem pelo template) → chamado em
     `src/main.c` logo após `xbox_MemoryLayoutInit`;
   - o modelo OHCI é opt-in → `RECOMP_USB=1`;
   - o alocador contíguo entregava físico `0x00082000` (HCCA) / `0x000826A0` (EDs), números que
     caem dentro da imagem (`0x10000..0x4FD000`); `bus_resolve()` em `ohci.c` trata endereço
     dentro da imagem como imagem e lia o `.text` como descritores USB
     (`ED 000826A0 info=FD8FDBE8 …`) → `RECOMP_CONTIG_AVOID_IMAGE=1`.
   Diagnóstico: `RECOMP_USB_TRACE=1 RECOMP_USB_STATS=1 RECOMP_INPUT_DIAG=1`. Com tudo certo o
   log mostra `[INPUT] kbd_env=1 … InputGetState=0 buttons=0x....` uma vez por segundo.

8. **Congelamento ao entrar no lobby** (Terminal Area: Home Server), thread principal girando em
   `sub_002B1270` (`0x002B1290`): o D3D espera `PGRAPH 0x400B10` bits 2–6 == semáforo & 0x1F
   (o latch do método `0x0310`), que o toolkit só escreve com `RECOMP_PB_WALK=dma`.
   - `RECOMP_PB_WALK=dma` **não serve aqui**: corrompeu memória e derrubou o jogo ao pular o FMV
     (`[ICALL] Failed … 0xFFFFDxxx`, SIGFPE em `sub_000706E0`). Não usar.
   - Correção: patch 4.4 (latch acompanha o semáforo no modo de varredura por segmento) +
     hook do método de software 5 em `d3d_fence.c`: quando o D3D julga a GPU muito atrasada ele
     grava `NO_OPERATION(5)` no pushbuffer e dorme em `KeWaitForSingleObject(device+0x1DCC)`
     (`0x002B1648`); no console a GPU interrompe e o handler sinaliza o evento. O runtime não
     levanta essa interrupção, então o executor chama o hook e o evento é sinalizado direto
     (`xbox_KeSetGuestEvent`). Rede de segurança: com a GPU ociosa (`PUT == GET`) e o evento não
     sinalizado, a thread observadora sinaliza.

`D:\Default_analysis.json`: o JSON do `xbe_parser` ficava em `game_data/` e o jogo o enxergava
ao enumerar `D:\`. Movido para `analysis/` (o `regen.sh` passa `--analysis-json`).

## 4. Patches no toolkit (`patches/xboxrecomp-dw4.patch`, base `37b61d5` v0.18.0)

Aplicados direto em `./xboxrecomp`; `git -C xboxrecomp diff` mostra o estado atual.
Para reaplicar num clone limpo: `git -C xboxrecomp apply ../patches/xboxrecomp-dw4.patch`.
Os três são bugs genéricos do backend POSIX x86-64, candidatos a PR upstream.

### 4.1 `src/kernel/kernel_bridge.c` — `bridge_va_mapped()`
Só aceitava RAM (< 64 MB) e a janela contígua. Agora aceita páginas `MEM_COMMIT` da VMA
estendida (`guest_vmem_query`).

### 4.2 `src/kernel/xbox_memory_layout.c` — `ac97_arm_write_trap()`
O ramo POSIX (`mprotect`) só era compilado em aarch64; em x86-64 Linux caía no ramo Win32
(`AddVectoredExceptionHandler`, que no POSIX é um stub que devolve NULL) → "could not arm the
bus-master write trap" e o reset de canal do DirectSound girava para sempre.

### 4.3 `src/platform/mmio_decode_x86_64.h` — operações ALU em MMIO
O decodificador só conhecia formas de `MOV`. O Clang funde a leitura com a operação
(`F7 84 03 …` = `test dword [mmio], imm32` em `sub_002EDEFF`), o que virava SIGSEGV no primeiro
registrador da APU. Acrescentado: `ADD/OR/ADC/SBB/AND/SUB/XOR/CMP` (00–3B, 80/81/83),
`TEST` (84/85, F6/F7 /0), `NOT/NEG` (F6/F7 /2,/3), `INC/DEC` (FE/FF /0,/1), **com atualização de
EFLAGS** no `ucontext` (a instrução seguinte é o `jcc`).

### 4.4 `src/kernel/nv2a_pb_exec.c`, `nv2a_backend.h`, `kernel_bridge.c` — fence do D3D
- ao escrever o semáforo (`0x1D70`) fora do modo DMA, espelha os bits de fence em `0x400B10`;
- `nv2a_pb_set_software_method_hook()`: callback para `NO_OPERATION` com valor ≠ 0;
- `xbox_KeSetGuestEvent(va)`: sinaliza, do host, um KEVENT montado pelo título.

### 4.8 `nv2a_pb_exec.c` — matriz de textura da função fixa
`ffp_vertex()` aplica `SET_TEXTURE_MATRIX` (`0x06C0 + 0x40·estágio`) quando
`SET_TEXTURE_MATRIX_ENABLE` (`0x0420 + 4·estágio`) está ligado, na convenção da matriz composta
(`out[i] = Σ M[4i+j]·in[j]`, q = 1 por omissão). `RECOMP_NO_TEXMATRIX=1` desliga para comparar.
Texgen (`0x03C0…`) continua não implementado.

### 4.7 `nv2a_backend.h` / `nv2a_pb_exec.c` — z-bias (polygon offset)
`Nv2aRenderState` ganhou `poly_offset_enable/factor/bias`, lidos do espelho de registradores
(`0x0338`, `0x0384`, `0x0388`). O back end aplica com `glPolygonOffset(factor, bias)`, como o xemu.
O rasterizador em CPU do toolkit continua sem z-bias.

### 4.6 `nv2a_backend.h` / `nv2a_pb_exec.c` — `Nv2aBatchExt` (combiners)
Lotes transformados (FFP e vertex program) passam a levar `Nv2aBatch.ext`: o `Nv2aCombiner`, as
quatro texturas, e por vértice quatro conjuntos de coordenadas de textura (s,t,r,q), a cor
especular e o fator de fog. Sem isso o back end só podia fazer "texel do estágio 0 × difusa".
Mesma ideia do patch dos ports Amped 2 / SSX 3.

### 4.5 `nv2a_backend.h` / `nv2a_pb_exec.c` — paleta de texturas P8
`Nv2aTexture` ganhou o campo `palette` (endereço resolvido da tabela de 256 cores) e
`nv2a_backend_decode_texture` passou a usá-lo; antes zerava a paleta e recusava toda textura
formato `0x0B`, que é a maior parte do que o Alchemy desenha.

### 4.9 `src/input/xinput_device.c`, `xinput_xbox.h`, `src/video/fb_present_sdl.c` — plugar o controle fechava o jogo (2026-11-24)

**Sintoma:** conectar o controle com o jogo aberto → o processo aborta com
`malloc(): unaligned tcache chunk detected`, sem relatório `[CRASH]` no log.

**Como foi achado:** `coredumpctl info <pid>` (o systemd guarda o core). Thread que abortou:
`ohci_run_periodic_list → usb_gamepad_report → controller_for_port_locked →
refresh_controllers_locked → SDL_GameControllerOpen → malloc`. Thread da janela, no mesmo instante:
`xbox_HostWindowMain → SDL_WaitEventTimeout → (tradução do evento "joystick conectado") →
função 0x2c000 do sdl2-compat`, parada no lock de joysticks **depois** de já ter dado `free` na lista.

**Causa:** no CachyOS a SDL2 é o `sdl2-compat` (API da SDL2 sobre a SDL3). Ele mantém uma lista
global de joysticks que `SDL_NumJoysticks()` libera e refaz; a tradução do evento de
conexão/remoção, na thread que bombeia eventos, faz o mesmo — e dá o `free` **antes** de pegar o
lock (visto no desmonte: `free(lista)` → `SDL_GetJoysticks()` → guarda). O toolkit chamava
`SDL_NumJoysticks()` na thread do USB emulado a cada relatório enquanto a porta estava sem
controle, ou seja, o tempo todo para quem joga no teclado. Ao plugar, as duas threads liberavam o
mesmo bloco (double free entre dois tcaches).

**Correção:** com janela, abrir/fechar controles acontece **só na thread que bombeia eventos**
(`xbox_InputSdlDeviceEvent`, chamada por `fb_present_sdl.c` a cada evento e uma vez ao começar);
as outras threads só leem o controle já aberto. Sem janela (headless) continua como antes: a
própria thread que lê também bombeia. O log ganhou `[PAD] port N <- "nome"` / `unplugged`.

**Teste:** `scripts/test_pad.sh` (`tests/pad_hotplug.c`, ideia do `test_pad.sh` do Amped 2/SSX 3):
controle virtual da SDL plugado/desplugado 400× com outra thread lendo sem parar — sem jogo, sem
janela, sem hardware. Código antigo: aborta com a mesma mensagem em 8 de 8 execuções. Corrigido:
3 de 3 passam (400/400 conexões vistas). No jogo: `tests/scenarios/hotplug.txt` (passo `vpad` do
roteiro). **Não verificado com controle de verdade** — só o usuário pode confirmar.

O pacote de release leva a SDL2 "de verdade" (`pysdl2-dll`), não o sdl2-compat; se ela tem a mesma
corrida não foi verificado, mas a correção vale para as duas.

## 5. Hooks (`src/hooks/`)

| Arquivo | Função |
|---|---|
| `recomp_manual.c` | `recomp_lookup_manual` (vazio por enquanto), logs de ICALL, `recomp_unimpl`, **defaults de ambiente** |
| `d3d_fence.c` | informa ao executor o endereço do semáforo da GPU |
| `screenshot.c` | `DW4_SHOT=<prefixo>` grava BMPs do scanout (depuração sem olhar a janela) |
| `input_script.c` | `DW4_INPUT_SCRIPT="22:enter 30:z 40:d*3"` injeta teclas por tempo (mesma tabela do teclado) |
| `nv2a_gl_backend.c` | renderizador OpenGL 3.3, compilado como `libdw4_gl.so` (recarga a quente) |
| `nv2a_vk_backend.c`, `shaders/` | renderizador Vulkan, compilado como `libdw4_vk.so` (§7.5) |
| `gl_host.c`, `gl_module.h` | escolha da API (`DW4_RENDERER=opengl\|vulkan\|cpu`), contexto EGL, carga/recarga do renderizador, `DW4_SW_RENDER=1` |
| `stall_watch.c` | amostrador de travamento (`DW4_STALL_MS`) |
| `dw4_hooks.h` | `LOG_ONCE`, protótipos |

Defaults aplicados pelo binário (qualquer `RECOMP_*` já definido no ambiente vence):
`RECOMP_PB_EXEC=1`, `RECOMP_FB_WINDOW=1`, `RECOMP_VBLANK=paced`, `RECOMP_EXT_VMA=1`,
`RECOMP_TITLE_KEVENTS=1`, `RECOMP_AC97_READY=1`, `RECOMP_USB=1`, `RECOMP_CONTIG_AVOID_IMAGE=1`,
`RECOMP_FLIP_PACING=1`, `RECOMP_STRIP_ALT_WINDING=1`.

## 6. Rede / Xbox Live / FMV

- **Rede:** o XBE não linka XONLINE nem XNET e não importa nada de rede — não há o que stubar.
- **FMV:** os vídeos são CRI Sofdec dentro de `ndmw.afs`; o decodificador (seções `PSFD_*`) foi
  recompilado junto e **funciona** (logos Sofdec/ADX, abertura e demos tocam). Nenhum stub de
  "pular vídeo" foi necessário.

## 7. Vídeo: executor de pushbuffer + back end OpenGL 3.3

O jogo linka o D3D8 do XDK estaticamente; nunca chama uma API D3D8 que o `d3d8_gl.c` possa
interceptar — escreve métodos NV2A num pushbuffer. O executor do toolkit (`nv2a_pb_exec.c`)
decodifica, transforma (FFP e vertex programs na CPU) e recorta; `src/hooks/nv2a_gl_backend.c`
recebe `clear`/`draw`/`flip` pela `nv2a_backend.h` e rasteriza na GPU.

- **Contexto:** EGL offscreen pela plataforma *device* (`EGL_EXT_platform_device`), 3.3 core,
  criado na thread do guest e tornado corrente na thread NV2A na primeira chamada. Recusa
  renderizador por software, salvo `DW4_GL_ALLOW_SW=1`.
- **Superfícies:** um FBO por `color_va`; depth/stencil `DEPTH24_STENCIL8` por `zeta_va`. A
  interface só dá o retângulo de clip, então a largura vem de `pitch/bpp` e a altura é arredondada
  para potência de dois quando a largura é (alvo 512×512 com clip 498×498+7+7 no lobby).
- **Vértices:** pixels da superfície → NDC no vertex shader, `w = 1/rhw` (perspectiva correta),
  `z / zmax` (16777215 para Z24, 65535 para Z16, decidido por `SET_CLIP_MAX`), `GL_DEPTH_CLAMP`.
  Linha 0 do FBO = linha 0 da superfície, então o `glReadPixels` cai direto na memória do guest.
- **Texturas:** `nv2a_backend_decode_texture` → BGRA8; cache por (endereço, tamanho, formato,
  paleta); hash amostrado dos bytes do guest + paleta, conferido 1×/frame (o FMV reescreve no lugar).
  Textura cujo endereço é uma superfície da GPU amostra a imagem do FBO (render-to-texture).
- **Estado:** blend, alpha test (no shader), depth, stencil, color mask e culling nos enums GL que
  a NV2A já usa.
- **Apresentação:** `present_sync` lê a superfície exibida de volta para a memória do guest; a
  janela SDL do toolkit, a entrada e o `DW4_SHOT` não mudaram.
- **Ritmo:** `RECOMP_FLIP_PACING=1` segura cada flip no vblank (60 Hz). Sem ele o jogo roda a
  milhares de fps na GPU e cai por corrupção de heap (corrida não localizada).
- **Combiners:** lotes com `Nv2aBatchExt` passam por um porte em GLSL de `rc_stage_fetch` +
  `nv2a_rc_eval`; lotes em espaço de tela continuam texel × difusa (como na CPU).
- **Lotes:** draws consecutivos com a mesma chave de estado (`DrawKey`) viram um `glDrawArrays`
  (lobby: ~4.800 draws → ~20 lotes; Commander Room: ~5.300 → ~25). 60 fps estáveis.
- **Paletas:** identificadas pelo conteúdo, conferido a cada uso.
- **Não feito:** mipmaps, scissor, cube maps, modos bump-env/dot-product dos estágios.

Referência: `bertibertig/SSX3-Recomp` e `bertibertig/Amped2-Recomp` (`src/render/vk_backend.c`,
`docs/vulkan-backend.md`, GPL-3) fazem o mesmo em Vulkan e estendem o `Nv2aBatch` com o estado dos
combiners. Nenhum código deles foi copiado. Desde 2026-11-24 temos o nosso back end Vulkan (§7.5).

### 7.0 Recarga a quente do renderizador (2026-11-24)

O renderizador é uma biblioteca à parte, `build/libdw4_gl.so` (`src/hooks/nv2a_gl_backend.c`),
carregada pelo host `src/hooks/gl_host.c` (interface em `gl_module.h`). Com o jogo aberto:

```bash
cmake --build build --target dw4_gl     # ou o build normal
```

No próximo flip o jogo vê que o arquivo mudou, carrega a versão nova e loga
`[GL] renderizador RECARREGADO (versao N)`. O estado do jogo não é tocado — quem está jogando
continua no mesmo lugar; a imagem pisca por um frame (texturas e superfícies são refeitas).

- O host é dono do contexto EGL; no reload destrói o contexto (todos os objetos GL do módulo velho
  vão junto) e cria outro. O módulo não guarda nada que precise sobreviver.
- A biblioteca é copiada para `build/gl_live/` com nome único antes do `dlopen` (o mesmo caminho
  devolveria a cópia antiga); a antiga fica mapeada (poucos MB por reload).
- Se a nova não carregar (erro de link, `DW4_GL_MODULE_ABI` diferente, contexto não criado), o
  renderizador antigo continua e o motivo vai para o log. Erro de compilação nem troca o arquivo.
- `eglBindAPI(EGL_OPENGL_API)` é **por thread**: a thread NV2A precisa chamá-lo antes de criar o
  contexto novo (sem isso: `EGL_BAD_ATTRIBUTE`).
- Só vale para o que está em `nv2a_gl_backend.c`. Mudança em `gl_host.c`, no toolkit, nos outros
  hooks ou no `gl_module.h` (ABI) exige reiniciar o jogo.
- As opções do módulo (`DW4_GL_*`, estado das teclas F5–F11) voltam ao padrão a cada reload.
- `DW4_GL_NO_RELOAD=1` desliga. Validado sem janela: 2 reloads no meio do cenário do lobby,
  goldens conferem.

**Não é save state.** Estado de jogo salvo/restaurado como em emulador não é viável aqui: o código
do guest roda como código nativo, com a posição de execução de cada thread na pilha do processo.

### 7.2 Teclas de diagnóstico e sonda (janela do jogo em foco)

F5 culling (normal/desligado/invertido) · F6 teste de alfa · F7 teste de profundidade ·
F8 combiners · F10 ignora draws só de profundidade · F11 ignora o z-bias · **F9** grava
`build/shots/f9-NNN.bmp` e `f9-NNN.txt`: um frame A normal e um frame B sem os draws só de
profundidade, com pixels pintados por draw (`[Q]`), alfa/paleta/UV da textura (`[QX]`), a sonda
`[PROBE]` do pixel central e o dump do primeiro mapa 32×32 (`[ATLAS]`).
`scripts/f9_diff.py` lista o que está escondido e por quem. `DW4_GL_F9_AT=<s>` dispara o mesmo
sem teclado (uso sem janela).

### 7.3 Ponte invisível no fim do tutorial e transição cinza do título — RESOLVIDO (2026-11-24)

**Causa única: o executor do toolkit não aplicava a matriz de textura do caminho de função fixa**
(patch 4.8). O Alchemy anima dissoluções deslizando o UV por matriz de textura: a malha lê um
único ponto de uma textura pequena (degradê de alfa ou tabela de cores) e a matriz move esse ponto.

- **Ponte:** ~500 ladrilhos (15×12 px, textura P8 32×32 `0x8215C000` = degradê de alfa, opaco nas
  linhas 0–8, transparente nas 24–31) desenhados **só em profundidade** (`cmask=0`, `zwrite=1`,
  alpha test `> 25`) formam uma tampa 0,0004 à frente do tabuleiro (6 quads, `0x82160000`,
  `zwrite=0`). Fim do tutorial = a matriz desliza o V de cada ladrilho até o alfa cair e a tampa
  some. Sem a matriz os ladrilhos liam para sempre a parte opaca.
- **Transição do título:** mesma técnica com uma tabela de cores 128×64; sem a matriz saía tudo
  cinza. Agora: circuito azul se desenhando, anel de glifos, clarão, túnel
  (`docs/title_transition_texmatrix.png`). O rasterizador em CPU do toolkit tinha o mesmo defeito.
- Validado sem janela na transição do título; **ponte e transição confirmadas pelo usuário jogando
  (2026-11-24).**

Como foi achado (vale como método): F7/F10 ao vivo → sonda do pixel central → captura F9 com
comparação A/B e contagem de pixels por draw (`scripts/f9_diff.py`) → estatística de alfa e dump
da textura. Descartados pelo caminho: z-bias (o jogo escreve `-0`), filtro linear × nearest,
paleta trocada no meio do frame, decodificação da P8.

### 7.4 Modelos 3D "salpicados" de preto na seleção de Digimon — RESOLVIDO (2026-11-24)

Dois defeitos somados, só visíveis onde o jogo liga culling (`cull=1(404,900)` no preview do
Digimon; o lobby desenha com culling desligado):
1. o executor emite as tiras de triângulos como `(i, i+1, i+2)`, então os ímpares chegam com o
   giro trocado e eram descartados → `RECOMP_STRIP_ALT_WINDING=1` virou default;
2. no renderizador a imagem fica espelhada na vertical dentro do GL (linha 0 do FBO = linha 0 da
   superfície), o que inverte o sentido de giro → o modo normal troca CW↔CCW ao passar o
   `front_face` (F5 → "giro cru" mostra o valor sem troca).
Só (1) deixava o modelo do avesso; (1)+(2) dá o modelo correto. Conferido sem janela nas três
combinações; referências atualizadas.

### 7.5 Back end Vulkan (2026-11-24) — `src/hooks/nv2a_vk_backend.c` → `libdw4_vk.so`

Segunda API gráfica, ao lado do OpenGL (que continua o padrão). O jogador escolhe no launcher
(Gráficos → API gráfica), que grava `renderer` em `launcher.json` e passa `DW4_RENDERER` ao jogo.

- **Seleção** (`gl_host.c`): `DW4_RENDERER=opengl|vulkan|cpu`. Se a API pedida não sobe (sem
  `libvulkan.so.1`, sem driver, sem placa acelerada), tenta a outra e por último a CPU. O log traz
  `[GL] renderizador ativo: <OpenGL|Vulkan> (...)`.
- **Interface** (`gl_module.h`, ABI 2): `name`, `needs_gl`, `init`, `shutdown` + os quatro
  callbacks. O módulo OpenGL usa o contexto EGL do host; o Vulkan cria e destrói o próprio
  dispositivo. A recarga a quente vale para os dois (`cmake --build build --target dw4_vk`).
- **O que veio dos ports de referência** (Amped 2 / SSX 3, só as ideias — são GPL-3):
  Vulkan-Headers fixado em `third_party/` (submódulo, v1.4.363), GLSL → SPIR-V em vetor C com
  `glslc -mfmt=c`, estado dos combiners num UBO por lote reaproveitado enquanto não muda.
- **O que é diferente deles, de propósito:**
  - *sem swapchain*: o frame é lido de volta para a memória do guest, como no OpenGL, e a janela
    SDL do toolkit apresenta. Janela, tela cheia, `DW4_SHOT`, soak sem janela e goldens funcionam
    iguais nas duas APIs;
  - *Vulkan 1.0 puro, sem extensões*: eles usam 1.3 + `VK_EXT_extended_dynamic_state3` + push
    descriptors. Aqui blend/depth/stencil/cull/máscara de cor entram num **cache de pipelines**
    (10 pipelines no lobby) e os descritores num cache por frame — roda em qualquer driver Vulkan.
- **Convenções:** y do NDC para baixo, então a linha 0 da imagem é a linha 0 da superfície e o
  sentido de giro da NV2A vale direto (o OpenGL precisa trocar CW/CCW, §7.4); z em [0,1];
  z-bias = `vkCmdSetDepthBias(constante = bias, slope = fator)`.
- **Frame:** dois command buffers, "upload" (texturas) e "draw", enviados nessa ordem no flip, com
  espera (um frame em voo — a leitura de volta precisa disso). Cada textura é reenviada no máximo
  uma vez por frame, no primeiro uso, por isso o upload pode rodar antes de todos os draws.
  Anéis por envio: 524 mil vértices, 8 MB de UBO, 16 MB de staging; cheio → envia e continua.
- **Superfícies** ficam em layout `GENERAL` (são alvo e textura no mesmo frame); uma barreira
  separa "desenhou" de "amostrou". Clear de só alguns canais sai como retângulo (o
  `vkCmdClearAttachments` ignora a máscara de escrita).
- **Shader:** `shaders/nv2a_vk.frag` é o mesmo porte de `nv2a_rc_eval` do OpenGL. **Correção num
  tem de ser repetida no outro.**
- **Opções:** `DW4_VK_DEVICE=<n>`, `DW4_VK_ALLOW_SW=1` (lavapipe), `DW4_VK_CULL=0|inv`,
  `DW4_VK_STATS=1`, `DW4_VK_DBG=<bits>` (os mesmos do `DW4_GL_DBG`); F5–F8, F10, F11. A captura F9
  e a sonda de pixel existem só no OpenGL — para investigar bug de imagem, use o OpenGL.
- **Validado sem janela** (RTX 5070, driver 620.73): `scripts/soak.py --vulkan --golden` — os 4
  frames de referência (gerados com OpenGL) conferem, lobby a 60 fps com ~4.000 draws → ~20 lotes;
  fallback Vulkan → OpenGL (`DW4_VK_DEVICE=99`); recarga a quente.
- **Não verificado:** jogar com janela; AMD/Intel (lá o depth é `D32F+S8`, o que muda a
  resolução do z-bias); camadas de validação (`vulkan-validation-layers` não está instalado).

### 7.1 Opções de depuração do renderizador

`DW4_GL_DBG` (bits): 1 sem alpha test, 2 sem blend, 4 sem depth, 8 sem combiners, 16 filtro nearest.
`DW4_GL_TRACE_AT=<s>` registra todos os draws de 2 frames a partir daquele instante (`[GLT]`).

## 8. Depuração e testes

### Rodar sem janela e classificar (ferramenta principal)

```bash
scripts/soak.py                      # cenário do lobby, 1 run, SEM janela (~95 s)
scripts/soak.py --runs 10            # soak
scripts/soak.py --golden             # e compara as capturas com tests/golden/
scripts/soak.py --scenario tests/scenarios/title.txt --timeout 60
scripts/soak.py -e RECOMP_GUEST_LOCK=1 -e DW4_STALL_MS=2000    # variáveis extras
scripts/soak.py --window | --sw      # com janela | rasterizador em CPU
```

- Sem janela = `RECOMP_HEADLESS=1`: o runtime não abre janela nem áudio; o back end OpenGL usa o
  contexto EGL offscreen, então a GPU trabalha igual. **Use isto para testar; não abre nada na tela
  do usuário.**
- Cada invocação fica em `build/soak/<data-hora>/NN/` (log, `shots/`, saves próprios);
  `build/soak/latest` aponta para a última. Nada é apagado. `save_data/` nunca é tocado.
- Veredito por run (via `scripts/triage.py`): `OK`/`AVISOS` (chegou ao `quit`), `CRASH`, `STALL`,
  `INCOMPLETO`. **"OK" só diz que o roteiro terminou — quem prova que a tela certa apareceu são os
  goldens** (`--golden`). Em 2026-11-23 dois runs "OK" tinham parado na tela de nome.

### Cenários (`tests/scenarios/*.txt`, sintaxe em `src/hooks/input_script.c`)

`DW4_INPUT_SCRIPT="@arquivo"` ou a lista direto. Passos: `f<N>:<tecla>` (no flip N — forma
recomendada, a lógica do jogo anda por frame), `wait_flips:<n>`, `<tecla>[*seg]`, `sleep:<s>`,
`shot:<nome>`, `mark:<texto>`, `quit`. `<tecla>!` (aperta e espera a tela mudar) existe mas é
**experimental e não confiável aqui**: os menus têm fundo animado.

Fluxo dos menus, uma tecla por tela (A = `z`, START = `enter`), conferido por captura:
título START → "Game Start" A → [carrega Entry] → A (Join) → START (Confirm) → A (OK?) → A (Yes) →
A (Create Digimon) → A (Dorumon) → A (letra) → START (nome pronto) → A → A (Yes, assign) →
START (log in) → lobby (~600 flips) → A avança o diálogo do Digi-Elf.
- A é ignorado durante transições; START não seleciona nos painéis.
- Não mexer no analógico nas telas Yes/No (o cursor vai para "No").

`tests/scenarios/lobby.txt` (teste, termina com `quit`) e `quickstart.txt` (usado por
`./quickstart.sh`, deixa o jogo aberto no lobby).

### Frames de referência (`tests/golden/*.bmp`, `scripts/golden.py`)

`scripts/golden.py <dir de shots>` compara; `--update` adota. Comparação tolerante: grade 32×24 de
cor média, passa com média ≤ 10 e pior célula ≤ 90 (fundos animados passam; tela branca/preta ou
cores trocadas não). Referências atuais: seleção do Dorumon, tela de login, lobby com diálogo,
lobby após 8 falas. Run a run a diferença medida foi média ≤ 0,5.

### Triagem de log

`scripts/triage.py <log>` → uma tela: render usado, fps (mín/média/máx), marcos do roteiro, alvos
de ICALL sem resolução (com a dica de semear), instruções não traduzidas alcançadas, arquivos
ausentes, bloco do crash ou do travamento, últimas linhas úteis, e o veredito.

### Travamento e crash, dentro do jogo

- **Amostrador de travamento** (`src/hooks/stall_watch.c`): sem flip por `DW4_STALL_MS` (padrão
  5000; 0 desliga) → `[STALL]` + despejo de todas as threads (`[DUMP]`: registradores e retornos do
  guest, backtrace com `sub_XXXXXXXX`); `[STALL] retomou` quando volta. Vale também para quem está
  jogando: o travamento chega diagnosticado no log.
- `scripts/dump_threads.sh [--busy]` — o mesmo despejo sob demanda (SIGUSR1), sem encerrar o jogo.
  Funciona com `ptrace_scope=1`.
- **Crash handler** (`src/main.c`): SIGSEGV/SIGBUS/SIGFPE/SIGILL/**SIGABRT** → registradores do
  guest, função (`-rdynamic`), retornos na pilha do guest, últimos ICALLs, backtrace do host.
- `scripts/gdb_threads.sh [seg]` — roda sob gdb e despeja todas as threads (abre janela).
- Build com AddressSanitizer no host: `cmake -S . -B build-asan -G Ninja -DCMAKE_C_COMPILER=clang
  -DDW4_ASAN=ON` e `scripts/soak.py --binary build-asan/dw4_recomp`.
- Log: stderr com buffer de 64 KB, descarregado a cada 250 ms (`DW4_LOG_UNBUFFERED=1` desliga).
  A linha `[GPU] draw #N` do toolkit só sai com `RECOMP_PB_EXEC_VERBOSE` (era 97% do log).
- Back end OpenGL: `DW4_GL_STATS=1`, `DW4_GL_TRACE_AT=<s>`, `DW4_GL_DBG=<bits>`, `DW4_SW_RENDER=1`.
- Toolkit: `RECOMP_WATCHDOG_SECS=N`, `RECOMP_KERNEL_LOG_BUDGET=N`, `RECOMP_WAIT_LOG=1`,
  `RECOMP_STUB_LOG=1`, `RECOMP_USB_TRACE=1`, `RECOMP_INPUT_DIAG=1`.
- `RECOMP_FB_DUMP` do toolkit não gravou nada nesta configuração; use `DW4_SHOT=<prefixo>`.
- O runtime grava `xbox_kernel.log` no diretório corrente.

### Observação não reproduzida (2026-11-23)

Num run sem janela com entrada errática (o detector `tecla!` apertando A/START fora de hora por
~140 s), o jogo passou a chamar alvos inválidos — primeiro `ICALL` para `0x00000000` com retorno do
guest em `0x00277037`, depois `0x00000100` em `0x00270421`, depois ~180 alvos-lixo — e a imagem
congelou. Não se sabe em que tela estava. Não reproduziu em 9 runs seguintes com roteiro fixo
(incluindo 2 com 35 s de pausa nos menus). O log se perdeu (o soak ainda apagava runs antigos; não
apaga mais). Se reaparecer: `scripts/triage.py` mostra o primeiro alvo e o ponto do roteiro.

## 9. Pendências conhecidas

- 470 instruções sem tradução (`RECOMP_UNIMPL`): quase todas são dados de `DSOUND` desmontados
  como código (`outsb/insd/arpl/bound`) e 3DNow! (`pfmul/pfadd`) num caminho alternativo de CPU;
  nenhuma foi alcançada até a tela título.
- 9 alvos de chamada não resolvidos (`recomp_stubs_unresolved.c`), nenhum alcançado.
- `AvSetDisplayMode`/PCRTC: `PCRTC_START` lê 0; a captura usa a superfície do `AvSetDisplayMode`.
