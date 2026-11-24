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

### 4.10 `src/video/fb_present_sdl.c` — frames de qualquer tamanho na janela (2026-11-24)

`xbox_FramebufferWindowPresentPixels(pixels, w, h, pitch)`: o renderizador entrega o frame pronto
(B,G,R,X) em vez de a janela ler os 640×480 da memória do guest. Os três slots de troca guardam
largura/altura; a textura e o `SDL_RenderSetLogicalSize` acompanham (letterbox na forma do frame),
a janela não-tela-cheia assume a proporção do frame mantendo a altura, e um frame que não é
640×480 é escalado com filtro linear. Depois da primeira chamada o present do flip (superfície do
guest) é ignorado. Usado pela resolução interna / widescreen (§7.6).

### 4.11 `src/kernel/xbox_memory_layout.c`, `nv2a_pb_scan.c` — a volta do anel do pushbuffer perdia comandos (2026-11-24)

**Sintoma:** "a tela pisca distorcida" por um quadro; mais frequente em 16:9 e em áreas pesadas
(usuário, Vulkan). No OpenGL o mesmo evento aparecia como "o chão pisca preto".

**Evidência** (caixa-preta, §7.7, `build/shots/flash-000..005`): no quadro suspeito o número de
draws é o mesmo dos vizinhos, mas o fundo sai com a textura errada (o atlas dos botões) e o log
mostra, no mesmo instante, um alvo de render-to-texture recriado com o estado da tela
(`superficie 81436000 640x480 ... (tela)` e logo depois de volta a `512x512`): o draw foi
executado com o endereço de cor de um alvo e o pitch/recorte de outro. Estado misturado =
comandos que não foram executados.

**Causa:** no modo padrão (varredura por segmento; `RECOMP_PB_WALK=dma` não serve aqui, §3.8) o
pushbuffer é um anel e, quando o PUT voltava para trás, o laço varria `[last_put, put_hi)` e
depois `[put_lo, put)`, com `put_hi`/`put_lo` = maior e menor PUT já vistos. Só que a volta
termina onde o jogo escreveu o JUMP e a seguinte começa no **alvo do JUMP**, o primeiro byte do
anel, que fica abaixo de qualquer PUT: tudo o que o jogo escreveu antes do primeiro kick da volta
(`[alvo, put_lo)`) nunca era executado, nem o fim de uma volta que passasse de `put_hi`. Cada
volta perdia alguns comandos (uma troca de alvo, um bind de textura) e um quadro saía com estado
velho. Mais draws por quadro = mais voltas por segundo.

**Correção:** na volta, varre a partir de `last_put` até achar o JUMP (a varredura para nele) e
continua do alvo do JUMP até o PUT, como o motor de DMA do hardware. Se o jogo reiniciou o anel
(escreve GET junto com o PUT), não há JUMP: recomeça do GET dele. Sem JUMP utilizável, cai no
comportamento antigo e loga `[PB] wrap without a usable JUMP`.

**Verificado sem janela (2026-11-24):** `soak.py --golden` e `--vulkan --golden` passam; em
1440p 16:9 (Vulkan e OpenGL, 3 execuções do cenário do lobby) a caixa-preta não registra nenhum
clarão e — o sinal mais direto — **sumiram as recriações transitórias de superfície**
(`806AC000 640x512 (clip 512x512)` ao entrar no lobby, que antes apareciam em toda execução):
o estado misturado não acontece mais. Nenhum `[PB] wrap without a usable JUMP`.
O cenário `title.txt` caiu em 2 de 4 execuções, com a assinatura do crash intermitente já
conhecido de pular a abertura (`[ICALL] alvo 0x00000000 ... ret guest 0x0026A1BE`); a taxa
histórica é 2/8, 2/3, 0/6 — não dá para dizer que mudou. **Falta o usuário confirmar jogando.**
Hipóteses descartadas
antes: shaders/pipelines; recriação de superfície como causa (é consequência); escolha da
superfície exibida; algo específico do renderizador Vulkan.

### 4.12 `src/kernel/nv2a_pb_scan.c` — método cortado por um kick (2026-11-24)

**Sintoma:** um pedaço do chão fica preto/escuro por um quadro, sutil (usuário, Vulkan e OpenGL).
Captura: `build/shots/flash-007` (F12), quadro 061 — um trecho do piso escurece e volta, com o
mesmo número de draws e triângulos dos vizinhos.

**Suspeita (não provada):** o D3D manda arrays grandes em vários kicks, então o PUT cai no meio
dos parâmetros de um método. A varredura por segmento largava o método no fim do segmento e lia o
resto dos parâmetros, no início do segmento seguinte, como cabeçalhos de comando (o modo `dma` já
tratava isso, `s_pend_*`). Resultado possível: uma malha desenhada pela metade ou com estado
errado. **Correção:** o segmento de topo guarda o método em que parou e o seguinte, se começa
exatamente onde aquele terminou, termina-o antes de continuar. O Vulkan loga
`[VK] pushbuffer: N metodos cortados por um kick e retomados` quando acontece.

**Estado (2026-11-24):** `soak.py --golden` passa nos dois renderizadores com o patch, mas no
cenário do lobby o contador ficou em **zero** — ali o D3D não corta métodos. A correção fica
(é o comportamento certo), mas provavelmente **não é a causa do chão**.

**O que a captura mostra de fato** (zoom e diferença entre os quadros 060/061/062 de
`flash-007`): vários ladrilhos inteiros em volta do personagem escurecem juntos por um quadro,
com a câmera parada e 0 decodes de textura. É o padrão do passe que multiplica o chão pela
textura de sombra projetada (alvo 512×512, limpo de branco e redesenhado todo quadro; no lobby:
`c5:F0:FFFFFF d5x77 d0x320t5`). Hipótese de trabalho: nesse quadro o alvo de sombra é amostrado
com conteúdo errado/escuro. Para confirmar, a etiqueta de cada quadro na caixa-preta agora traz a
sequência de operações (`cN` clear, `dNxM` M draws na superfície N, `tK` amostrando a superfície
K): comparar o quadro ruim com os vizinhos. Outra hipótese anotada: Outra hipótese anotada: passes coplanares com `LEQUAL` / z-bias dependente da resolução
(o termo de inclinação do polygon offset encolhe com a escala da imagem).

### 4.13 `xbox_memory_layout.c`, `kernel_bridge.c`, `nv2a_pb_exec.c`, `nv2a_pb_scan.c` — esperas que queimavam dois núcleos (2026-11-24, DW4R-2)

**Sintoma:** ~195% de um núcleo no lobby, qualquer que fosse a carga (`PORT_NOTES` §7.10).

**Como foi achado:** amostrador próprio (`DW4_PROF`, `src/hooks/prof.c`; a máquina não tem `perf`).
A thread do jogo passava **86%** das amostras num laço de 2 instruções em `sub_002B1320`
(kick-off do D3D: liga o bit 16 de `NV_PFB 0x100410`, gira até a GPU limpar, grava `DMA_PUT`); a
thread `nv2a_ack_thread` passava 57% em `sched_yield` + leitura de relógio (laço terminado em
`Sleep(0)`). O bit só é limpo entre duas caminhadas do pushbuffer e não é limpo com um flip
retido à espera do vblank — é assim que o jogo "sente" o FLIP_STALL.

**Correção (três partes):**
- *thread de ack dorme:* durante um flip retido espera um evento que o timer de vblank sinaliza
  (`xbox_Nv2aAckWake`, chamado em `kernel_vblank_count`); com o pushbuffer quieto espera o próximo
  kick. Logo depois de trabalhar continua cedendo a vez por `RECOMP_ACK_HOT_US` (300; 0 em
  máquina de 1–2 núcleos). Teto de 1 ms por espera. `RECOMP_ACK_SLEEP=0` volta ao `Sleep(0)`.
- *portão do kick:* `sub_002B1320` é embrulhada em `recomp_manual.c` (`sub_002B1320_gen` = corpo
  gerado): `xbox_Nv2aKickGate()` antes, `xbox_Nv2aKickDone()` depois. O portão dorme até tudo o
  que o jogo já enviou ter sido caminhado e nenhum flip estar retido — exatamente quando o laço
  do jogo é respondido na hora. Contagem de kicks lida pela thread de ack **antes** de ler o PUT
  (o jogo incrementa depois de gravar o PUT), publicada como "feito" quando o PUT foi consumido.
  `DW4_KICK_GATE=0` desliga. O jogo envia nem antes nem depois do que enviava: só a espera mudou.
- *limitador de 30 fps:* `xbox_Nv2aHostStall(1/0)` em volta do sono (`fps_limit.c`); o portão
  espera junto.

**Medido** (cenário do lobby, 92 s, sem janela, Vulkan, RTX 5070 / 16 threads): CPU do processo
178,8 s → **25,5 s** (195% → 28% de um núcleo), mesmos flips nos mesmos instantes, goldens iguais.
A ideia de recuar quando o pushbuffer está quieto é do Amped 2 (`RECOMP_ACK_IDLE_US`, um `usleep`
depois de 4000 voltas paradas); lá o flip retido e a thread do jogo continuam girando.

**Modo paralelo (`RECOMP_KICK_AHEAD=1`, desligado por padrão; launcher: Gráficos → Desempenho):**
medido com o portão: ~14 kicks por frame, e a thread do jogo dorme **3,8 ms por frame esperando o
executor** terminar o trecho anterior — jogo e executor se revezam. Com a opção o portão só
espera a cadência (flip retido / limitador), marca o kick como concedido e ele é respondido de
dentro da caminhada (`xbox_Nv2aAckGranted`, a cada 64 palavras) ou do laço de ack, mesmo com flip
retido. Lobby: espera pelo executor 3,8 → 0,0 ms por frame, 2 execuções com goldens iguais e a
mesma cadência (600 holds / 10 s). O quanto o jogo se adianta passa a depender só do D3D dele
(fence do swap, espera pelo `DMA_GET`), como no console. **Não validado além do lobby**: se um
buffer dinâmico for reescrito antes de o executor ler, o sintoma seria geometria/textura piscando.

Também neste patch: `getenv("RECOMP_PB_EXEC_VERBOSE")` era chamado a cada draw (2,6% da thread);
`zbuf_clear` do rasterizador em CPU rodava a cada clear mesmo com back end de GPU (2%);
`raster_xf_clipped` copiava os 3 vértices inteiros antes de saber se o triângulo precisava de
recorte. Estatística: `xbox_Nv2aKickStats` (linha `[VK] jogo: ...` a cada 5 s).

### 4.14 `src/video/fb_present_sdl.c` — janela para superfície Vulkan (2026-11-24, DW4R-2)

`RECOMP_WINDOW_VULKAN=1` (default quando o renderizador é Vulkan, `recomp_manual.c`): a janela é
criada com `SDL_WINDOW_VULKAN` e **sem** `SDL_Renderer`. O back end pede as extensões de instância
e a superfície (`xbox_FramebufferWindowVkExtensions` / `VkSurface`), cria o swapchain e apresenta
da thread do executor; a thread principal continua com eventos, título e forma da janela
(`xbox_FramebufferWindowFrameShape`). Enquanto ninguém segura a superfície (`VkRelease`, ou o
Vulkan não subiu) os frames chegam pelo caminho antigo e o laço cria o `SDL_Renderer` no primeiro
frame (testado no gamescope com `DW4_VK_DEVICE=99`: cai no OpenGL e o log mostra
`no Vulkan presenter: SDL renderer after all`; a imagem nesse caso não foi vista).
Ao fechar com um swapchain vivo a janela é deixada para o `exit()`.

### 4.15 `nv2a_backend.h`, `nv2a_pb_exec.c` — origem do recorte da superfície (2026-11-24, DW4R-2)

`Nv2aSurface.clip_x` / `clip_y` (de `s_gpu.clip_x/y`, pixels reais). A interface só dava o
tamanho do recorte; sem a origem o back end não tinha como cortar um sub-viewport (o minimapa das
fases, `PORT_NOTES` §7.6).

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

### 7.5b Apresentação direta e quadros em voo no Vulkan (2026-11-24, DW4R-2)

O "sem swapchain" acima deixou de ser verdade com janela. Dois modos (`[VK] apresentacao: ...`):

- **direto** (com janela, padrão): swapchain na janela do toolkit (patch 4.14). No flip, um
  `vkCmdBlitImage` leva a superfície exibida para a imagem do swapchain (escala e faixas pretas
  na GPU; `NEAREST` em 480p original, `LINEAR` em resolução interna maior) e o envio **não
  espera**: há dois conjuntos de frame (`FrameSet`: command buffers, anéis de vértices/UBO/staging,
  descritores, semáforos) e o executor só espera se a GPU estiver um frame inteiro atrasada.
  `frame_submit()` sem "async" continua esperando todos os conjuntos — é o ponto seguro para
  destruir imagens. A memória do guest não recebe o frame (como já era em resolução interna).
- **leitura** (sem janela, `DW4_VK_PRESENT=readback`, ou swapchain indisponível): o de antes.
- `DW4_VK_PRESENT=async`: quadros em voo sem janela, para `soak.py --golden` exercitar o caminho.

**Captura sob demanda** (`screenshot.c`): no modo direto o frame só é lido de volta quando alguém
pede (`dw4_frame_wanted()`: captura do roteiro, `DW4_SHOT`, assinatura, F9, caixa-preta ligada).
A **caixa-preta de quadros (§7.7) passou a ser opcional: `DW4_FRAME_RING=1`** — ligada, obriga a
ler todo frame e monta uma linha de texto por lote.

**Vsync:** `DW4_VSYNC=auto|on|off` (o launcher manda `RECOMP_PRESENT_VSYNC=0` com o vsync
desligado). auto = `MAILBOX` se existir, senão `FIFO`; off = `IMMEDIATE`, senão `MAILBOX`.
O jogo já é cadenciado pelo vblank emulado, então isto só decide o que fazer com um quadro fora
de fase com o monitor.

**Extensões:** só as de WSI (`VK_KHR_surface` + plataforma + `VK_KHR_swapchain`), e só para
apresentar; sem elas vale o modo leitura, que segue Vulkan 1.0 puro. Escolha de placa: com
janela, quem apresenta nela ganha de uma mais forte que não apresenta.

**Medido** (lobby, 3840×2160 16:9, janela dentro de `gamescope --backend headless`, RTX 5070):

| | fps | ms/frame no renderizador | CPU do processo |
|---|---|---|---|
| leitura (antes) | 49 | 15,9 (GPU 10,7 + cópia p/ janela 4,3) | 93% de um núcleo |
| direto | 60 | 1,2 | 34% |

**Verificado:** `soak.py --gamescope --golden` (janela + swapchain, 480p) e `--golden -e
DW4_VK_PRESENT=async`; 4K 16:9 com `DW4_VK_SHOT_SWAP=1` (captura da própria imagem do swapchain:
escala e forma corretas; a janela é redimensionada e o swapchain refeito). **Não verificado:**
janela real no monitor, tela cheia, minimizar, trocar de monitor, AMD/Intel, X11 puro, recarga a
quente no modo direto, e o OpenGL continua no caminho de leitura (não ganhou nada disto).

Das referências: todos os ports Vulkan apresentam por swapchain; o nosso mantém o modo leitura
como reserva e para os testes sem janela, e fica sem extensões fora do WSI.

### 7.6 Resolução interna e widescreen (2026-11-24) — `src/hooks/render_opts.h`

`DW4_RES=<altura>` (480, 720, 1080, 1440, 2160) e `DW4_ASPECT=<L:A>` (16:9, 16:10, 21:9; 4:3 =
original); no launcher, Gráficos → Imagem. Vale para os **dois** renderizadores (o cabeçalho é
comum). Desenho tirado do Amped 2 e dos dois SSX 3 (`docs/REFERENCIAS.md`), código nosso.

- **Resolução:** a imagem da GPU tem o tamanho da superfície do guest × (altura/480), para todas
  as superfícies (alvos de render-to-texture também). Os vértices continuam em pixels do guest e o
  shader divide pelo tamanho do guest: só mudam tamanho de imagem, viewport e área de clear.
- **Proporção:** superfícies "de tela" (640 de largura, ≥ 400 de altura) ficam `wide` vezes mais
  largas, e todo vértice desenhado nelas tem o x comprimido em torno do centro por 1/`wide`
  (`ro_squeeze`, na CPU, ao copiar o lote). O que o jogo desenhava além das bordas aparece nas
  laterais (Hor+, sem patch no jogo: o executor só recorta no near plane). O 2D (rhw = 1: HUD,
  menus, vídeos) fica em 4:3 no meio. Exceção: triângulo 2D que cobre a tela inteira (fade,
  clarão) continua cobrindo tudo.
- **Apresentação:** nesses modos o frame não cabe nos 640×480 do guest; vai direto para a janela
  (patch 4.10) e para as capturas (`dw4_frame_note_host`, `screenshot.c`). **A memória do guest
  não recebe mais o frame** — se algo no jogo ler o próprio framebuffer, vai ler lixo (não visto).
  Com 480 + 4:3 nada muda: caminho antigo, goldens iguais.
- **Armadilha achada:** ao entrar no lobby o jogo faz uma passada com recorte 512×512 ainda
  apontando para o back buffer; a superfície vira 640×512 para sempre. Com "tela = altura ≤ 480"
  ela deixava de ser alargada e a imagem saía 4:3 ou 16:9 conforme o buffer da vez. Por isso não
  há limite superior, e a tela exibida é `min(altura, 480)` linhas.
- **Custo:** a leitura de volta passa de 1,2 MB para 14,7 MB por frame em 1440p 16:9 (GPU → CPU →
  janela). Sem janela: 60 fps no lobby nos dois renderizadores (RTX 5070). Um swapchain próprio
  eliminaria as cópias; não foi feito para manter um caminho só para as duas APIs.
- **Culling do jogo (`src/hooks/widescreen.c` + gancho em `recomp_manual.c`):** o jogo só manda
  desenhar o que cabe no campo de visão de 4:3; objetos inteiros dentro da faixa lateral nova
  sumiam e surgiam conforme a câmera andava (usuário, 1440p 16:9; capturas `f9-011`/`f9-012`: o
  piso da plataforma à direita some, o anel da mesma plataforma — maior — fica).
  - O teste é **`sub_000BC050`** (igFrustCullTraversal): `ecx` = 8 cantos da caixa em espaço de
    clip (x, y, z, w; 16 bytes cada); devolve 2 = fora, 1 = dentro, 3/5 = parcial. Chamado por
    `sub_000BCC80` e `sub_000D7EC0`, com os cantos num temporário da pilha.
  - Correção: **gancho de entrada** `sub_000BC050_enter` multiplica o x dos 8 cantos por
    1/(`wide` × 1,05) antes do teste. Projeção e desenho não mudam; serve também às câmeras de
    sub-viewport. `DW4_CULL_SCALE=<f>` força outro alargamento (1 = original).
  - Gancho de entrada = recurso do recompilador: `scripts/regen.sh` agora passa
    `--exclude-manual src/hooks/recomp_manual.c`; criar/remover um gancho exige
    `scripts/regen.sh --lift` (~75 s) e build completo.
  - **Hipótese descartada:** `igFrustCullTraversal::_outerFrustCullScale` (campo +0x228; o setter
    em 0x000BCC2x grava o inverso no global 0x00397A90). Achado pelas tabelas de nomes de campo do
    Alchemy (`.data` 0x0039AA80, offsets em 0x0039AAB8). Gravar ali não resolveu: no fim de
    `sub_000BC050` a escala só reclassifica o caso parcial (3 → 5); quem está fora do campo de
    visão real continua fora (testado com o usuário, x1,373, o objeto continuou sumindo).
  - Os SSX 3 resolvem com hook na projeção da câmera; aqui só o descarte alarga.
  - **Compilado; ainda não executado nem confirmado pelo usuário.**
- **Medido em 4K (2026-11-24, sessão do usuário, Vulkan, 3840×2160, Central Area, limite 60):**
  30–48 fps, placa a ~60%. O log mostra o renderizador gastando **17–19 ms por quadro** sozinho
  (o orçamento a 60 fps é 16,7): ~9 ms esperando a GPU (desenho + cópia da imagem de 33 MB para
  a memória) e 5,6 ms copiando o quadro para a janela. Em 1440p eram 7,7 ms (4,1 + 1,4). Ou
  seja, em 4K o gargalo é a leitura de volta, não o desenho: apresentar direto da GPU (swapchain
  Vulkan na janela, como os ports de referência) tiraria ~14 ms por quadro. É o próximo passo de
  desempenho para resoluções altas; nas áreas pesadas sobra ainda o custo de CPU do executor.
- **Fade e escurecimento presos em 4:3 (2026-11-24, DW4R-2):** em widescreen, ao trocar de
  cenário aparecia um "quadrado 4:3" escuro no meio com o cenário visível nas laterais, e o
  escurecimento atrás de menus (ex.: portal do Commander Room) cobria só o meio.
  - *Como foi achado:* cenário de teste com capturas a cada 20 flips entre o login e o lobby em
    16:9 (reproduz sem janela), depois `DW4_FRAME_RING=1` + `ringdump` para listar os lotes do
    quadro exato. Hipóteses descartadas pelo rastreio (`DW4_RO_TRACE=1`): quadro 2D quase de tela
    inteira (existem — 29,6..611,4 de 640 com uv constante — e também foram tratados, mas não eram
    o quadrado); foto do próprio quadro como textura (o quadro 640×480 com uv 0..640 é o **vídeo**
    de abertura, textura 1024×512 em 0x81000000); malha 2D de quadradinhos (não existe).
  - *Causa:* o fade é uma **malha 3D presa à câmera** (1086 vértices, rhw constante
    0,00136..0,00138, blend 302/303, teste de z ligado), centrada na tela, com miolo escuro de
    ±424 px — o tamanho exato para cobrir 4:3 — e moldura clara até ±516 px. Espremida em x como
    o resto do 3D, o miolo virava o retângulo 4:3 e a moldura aparecia nas laterais.
  - *Correção (`ro_squeeze`, agora chamada no **flush do lote**, nos dois renderizadores):* lote
    em que todo vértice tem o mesmo rhw (±3%), caixa cobrindo a tela 4:3 inteira, centrada, e
    área somada ≥ 90% da tela não é espremido. Vale também para malha 2D. Quadro 2D de altura
    inteira, ≥ 70% da largura e uv constante é alargado até as bordas.
  - *Efeito colateral aceito:* a tela de "Now Loading" (faixas de texto em lote que cobre a tela)
    passou a ocupar a tela larga, esticada 1,33× em 16:9; antes ficava em 4:3 com faixas pretas.
  - *Risco:* um chão visto exatamente de cima (rhw constante, cobrindo a tela) deixaria de ser
    espremido e sairia esticado. Não visto.
  - Verificado sem janela em 16:9 nos dois renderizadores (fade de saída do login e de entrada
    no lobby); goldens 4:3 iguais. **Confirmado pelo usuário jogando (2026-11-24):** "melhorou
    muito os efeitos".
- **HUD ancorado nas bordas (2026-11-24, DW4R-2):** em tela larga o HUD (retrato, HP/MP, nível)
  ficava em 4:3 no meio, com a faixa escura acabando antes das bordas. Rastreio
  (`DW4_RO_TRACE=<flip>[,<flip>]` lista os lotes de um quadro): todo o 2D do jogo tem rhw = 1; o
  HUD é uma **faixa** de 3 quadros (-18..659 × 393..485, encostada na borda de baixo) seguida dos
  elementos do jogador; a caixa de diálogo é um painel -54..695 × 295..463 (não encosta na borda).
  Regra em `ro_squeeze`: lote 2D da largura da tela, baixo (≤ 40% da altura) e encostado na borda
  de cima ou de baixo = faixa, não é espremido; o lote 2D seguinte na altura da faixa e todo de
  um lado da tela é ancorado na borda desse lado (escala em x em torno da borda). Diálogo e menus
  continuam em 4:3. Verificado sem janela: Vulkan 16:9 e 21:9, OpenGL 16:10 (1 jogador, lobby).
  **Menu rápido de itens (captura `f9-014` do usuário, 16:9):** o painel e os textos foram para o
  canto, mas a barra de seleção (55..171 × 317..341, textura e blend próprios) ficou no meio —
  ela está toda acima da faixa e o teste era pelo centro do lote. Agora cada lado tem uma zona
  de altura que começa na faixa e **cresce com cada lote ancorado** (o painel, 317..440, é
  desenhado antes da barra). Não reproduz sem janela (no lobby, com personagem novo, o menu não
  abre): aguardando o usuário.
  **Não verificado:** 2–4 jogadores (um lote que junte elementos dos dois lados fica no meio),
  HUD de chefe / outros elementos de borda, fases.
- **Minimapa das fases enorme — recorte do guest não era aplicado (2026-11-24, DW4R-2):** o
  painel do mapa ocupava do meio da tela até além da borda direita. Não é do widescreen: vale em
  qualquer proporção.
  - *Como foi achado:* captura F9 do usuário no OpenGL (`build/shots/f9-011.txt`): os 70 draws do
    mapa vêm com `clip=120x120` na superfície 640×480, e o conteúdo é desenhado 5× maior (painel
    de 606×606 centrado em 588,134; salas até x = 721). A NV2A corta o que sai do recorte
    (`SET_SURFACE_CLIP_*`); os dois renderizadores só usavam o tamanho do recorte, sem origem e
    sem cortar.
  - *Correção:* `Nv2aSurface` ganhou `clip_x`/`clip_y` (toolkit, patch 4.15); a chave do lote
    guarda o recorte quando ele é menor que a superfície e o flush aplica scissor
    (`vkCmdSetScissor` / `glScissor`; o clear não é afetado). `DW4_GL_MODULE_ABI` = 3.
  - *Em tela larga* (`ro_clip_mode` / `ro_scissor` em `render_opts.h`): um recorte estreito é um
    elemento só — vértices e retângulo passam pela mesma transformação em x; encostado na borda
    lateral da tela 4:3 é ancorado na borda da tela larga (o minimapa vai para o canto), senão é
    espremido em torno do centro. Recorte da largura inteira só corta na vertical.
  - *Verificado sem janela:* goldens 4:3 iguais nos dois renderizadores (a seleção de Digimon usa
    um recorte para o modelo 3D e o alvo de sombra usa 498×498 em 512×512); 16:9 sem regressão.
    **Confirmado em 4:3 por captura F9 do usuário** (`build/shots/f9-012.bmp`): o minimapa
    aparece como um quadrado de 120×120 em 468..588 × 36..156, no canto superior direito. Essa
    posição mostrou que o limiar de "encostado na borda" (92%) não pegava o mapa; passou a 85%.
    **Confirmado pelo usuário jogando (2026-11-24, OpenGL):** 4:3, 16:9 e 21:9 normais.
    Depois também no Vulkan em 21:9.
- **ABERTO: menu do START quebra em 21:9** (usuário, Vulkan, 2026-11-24): aparece esticado; em
  16:9 está certo. Sem captura ainda (o F9 com lista de draws só existe no OpenGL); o usuário vai
  mandar. Suspeitas a conferir na captura: as regras de `ro_squeeze` com limiar em fração da
  largura (lote de tela inteira, faixa do HUD) pegando um painel do menu só em 21:9.
- **Limites conhecidos:** menus e vídeos ficam com faixas laterais; a sonda de pixel do F9 só
  funciona em 480 + 4:3; os frames de referência (`tests/golden`) só valem para 480 + 4:3.
- **Verificado sem janela:** 1440 + 16:9 nos dois renderizadores (seleção de Digimon, login,
  lobby); `--window` com `SDL_VIDEODRIVER=dummy` exercita o caminho da janela. **Não verificado:**
  janela/tela cheia de verdade (só o usuário), 16:10, 21:9, 2160, fases além do lobby.

### 7.7 Caixa-preta de quadros: defeito que dura um quadro (2026-11-24) — `screenshot.c`

Para "a tela pisca" (não dá tempo de apertar F9). Cada quadro apresentado é guardado reduzido
(até 640 de largura) num anel de 120 quadros (~2 s) e o anel é gravado em
`build/shots/flash-NNN/` (um BMP por quadro + `info.txt`):

- **sozinho**, quando um quadro difere muito do anterior (média > 14 na grade 16×12 de
  luminância) **e** o seguinte volta a parecer com o anterior — um clarão; corte de cena não
  volta. No máximo 6 por sessão (`DW4_FLASH_MAX`). Linha `[FLASH] flip N: ...` no log;
- **a pedido:** F12 na janela (Vulkan) ou o passo `ringdump` do roteiro.

`info.txt` traz, por quadro, o flip, a diferença para o anterior e a etiqueta do renderizador
(superfície exibida, draws, lotes, triângulos, texturas) — o quadro suspeito vem marcado.
`DW4_FRAME_RING=0` desliga. Custo: ~110 MB de RAM e uma cópia reduzida por quadro.

Também: o launcher liga `DW4_VK_STATS`/`DW4_GL_STATS` (uma linha a cada 5 s no log); no Vulkan a
linha traz `ms/frame: back end X (GPU Y, copia p/ janela Z)` e avisa se algum quadro exibido veio
de uma superfície que não é a tela. A escolha da superfície exibida passou a ser pela área da
superfície, não pelo recorte do último draw (um alvo 512×512 podia "ganhar" da tela por um quadro).

**Primeiro uso (2026-11-24):** 6 capturas automáticas numa sessão do usuário; levaram à causa da
piscada (patch 4.11: a volta do anel do pushbuffer perdia comandos).

### 7.8 ABERTO: o chão escurece por alguns quadros (2026-11-24)

Sintoma: na Central Area (e na caverna), o piso de ladrilhos escurece em padrão listrado por 1–7
quadros e volta; Vulkan e OpenGL; às vezes com o personagem andando. Capturas com detalhe por
lote: `build/shots/flash-016`, `-018`, `-020` (`lotes.txt`).

**O que as capturas provam** — nos quadros escuros, comparados com os vizinhos, é idêntico:
sequência de operações (clears, draws por superfície, uso do alvo de sombra), lotes, estado de
blend/alpha/depth, endereços de textura, paleta (endereço e conteúdo), cores de vértice,
especular, neblina (1,000 = sem neblina), cor de neblina, combiners finais, intervalo entre
flips (15,6–18,9 ms) e nenhuma textura criada ou alterada no quadro.

**Descartado, com evidência:** comandos perdidos (sequência idêntica), cadência de quadros, alvo
de sombra com conteúdo errado (lotes do alvo idênticos), neblina, paleta trocada, textura
recarregada, shaders/pipelines.

**O que sobra:** as coordenadas de textura. O resumo `uv=` dos lotes do piso muda a cada quadro
(animação por matriz de textura, patch 4.8 — os ladrilhos "pulsam"), então não dá para isolar o
quadro ruim por ele. Hipótese: nesses quadros a matriz de textura de alguns ladrilhos tem um
valor fora da sequência e eles amostram a parte escura da textura. Não se sabe se vem do jogo
(animação) ou do port. Coincide, em todas as capturas, com a mudança do número de draws
(conjunto de objetos visíveis mudando) um quadro antes ou depois.

**Também descartado depois** (`flash-021`, `flash-022`, em 480p 4:3, Vulkan; o usuário viu o
mesmo em OpenGL): não depende de widescreen, resolução interna nem do descarte alargado (nada
disso estava ativo); constantes do combinador (hash de todo o `Nv2aCombiner`, fatores, cores
finais, cor de blend) idênticas; matriz de textura do estágio 0 avança suave e monotônica pelos
quadros escuros, sem valor fora da sequência.

**O que ainda não foi medido:** profundidade (z dos vértices, valor do clear de depth, disputa
entre passes coplanares com `LEQUAL`) e o conteúdo real das texturas/alvos (só endereços e
hashes). **Padrão que se repete em todas as capturas:** o escurecimento vem 1–4 quadros antes
de um grupo novo de objetos entrar (+300 draws) — troca de setor do mapa.

**Decisão do usuário (2026-11-24): fica como defeito conhecido não resolvido.** Para retomar:
registrar z mínimo/máximo por lote e o valor do clear de depth na caixa-preta; comparar com o
rasterizador em CPU (`--sw`) num cenário que chegue à Central Area.

Achado lateral corrigido: texturas não paletizadas eram chaveadas no cache pela paleta "ligada"
no momento (campo `palette` velho) e ganhavam cópias novas quando ela mudava; agora só P8.

### 7.9 Limitador de quadros (2026-11-24) — `src/hooks/fps_limit.c`

`DW4_FPS_LIMIT=30` segura cada flip até 1/30 s do anterior. Launcher: Gráficos → Imagem → Limite
de quadros, **padrão 30**; `run.sh` também usa 30. O binário sem a variável roda a 60 (é como o
`soak.py` roda).

- **A lógica do jogo anda por quadro, não por tempo.** Medido: com o limite em 30,
  `soak.py --vulkan --golden` mostra as mesmas telas nos mesmos flips e leva 171 s em vez de 92 s.
  A 60 fps tudo anda duas vezes mais rápido que a 30.
- **Por que 30 é o padrão:** o usuário, que conhece o jogo no console, afirma que no Xbox ele
  rodava a 30 (2026-11-24). O binário pede intervalo de swap ONE (um flip por vblank — contador
  `iv1` em `[GPU] flip hold`), mas com vsync um quadro atrasado espera o vblank seguinte, então
  um jogo pesado para o console fica em 30. **Não foi medido em hardware real.**
- Até aqui todo o port foi testado e jogado a 60, ou seja, no dobro da velocidade do console.
- Observação do usuário: a 30 fps o defeito do chão (§7.8) fica menos brusco e demora mais a
  aparecer. Compatível com o defeito durar um tempo real fixo (carga do setor) e não um número
  de quadros; não investigado.

### 7.10 Vulkan × OpenGL: custo medido (2026-11-24)

Mesmo cenário (`soak.py`, menus → lobby, 92 s, sem janela), RTX 5070; CPU = tempo de processo
(user + sys), GPU = média do `nvidia-smi`:

| | CPU | GPU |
|---|---|---|
| Vulkan 480p 4:3 | 178,8 s (195% de um núcleo) | 5%, 21 W |
| OpenGL 480p 4:3 | 183,5 s (200%) | 5%, 20 W |
| Vulkan 1440p 16:9 | 167,2 s (182%) | 18%, 36 W |
| OpenGL 1440p 16:9 | 183,3 s (199%) | 18%, 41 W |

Uma execução de cada: a diferença de 3% em 480p está dentro do ruído; os 9% em 1440p
provavelmente não. O custo é de CPU (jogo + executor, ~2 núcleos) e quase independe da API. É
daí que vêm os textos da tela Gráficos do launcher. Vulkan virou o padrão por decisão do usuário.

Janela: o launcher não oferece mais "tamanho da janela"; passa `RECOMP_WINDOW_SIZE=LxA` (patch
no toolkit) com a resolução interna (dobrada em 480p/720p), e a janela é reduzida se não couber
na área útil do monitor.

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

### 8.x Perfil de CPU sem `perf` (2026-11-24) — `src/hooks/prof.c`, `scripts/prof_report.py`

`scripts/soak.py -e DW4_PROF=/tmp/p.txt -e DW4_PROF_FROM=3900` grava amostras (SIGPROF a 1000 Hz
de CPU do processo, a partir do flip dado) e `scripts/prof_report.py /tmp/p.txt` mostra o consumo
por thread e as funções mais amostradas (`sub_XXXXXXXX` = jogo). `soak.py` agora imprime o tempo
de CPU de cada execução. `soak.py --gamescope [LxA]` roda com janela dentro de um gamescope sem
tela (nada aparece no monitor) — o único jeito de exercitar o swapchain sem o usuário.

Perfil do lobby depois da DW4R-2 (28% de um núcleo no total): executor ~68% (decodificação e
transformação de vértices do toolkit: `draw_primitive`, `fetch_attr`, `backend_from_xf`,
`raster_xf_clipped`; o renderizador Vulkan é ~6%), jogo ~25%, áudio ~5%. O próximo ganho de CPU
está no executor do toolkit (cache de vértices pós-transformação, como SSX 3 / Amped 2 fizeram).

## 9. Pendências conhecidas

- 470 instruções sem tradução (`RECOMP_UNIMPL`): quase todas são dados de `DSOUND` desmontados
  como código (`outsb/insd/arpl/bound`) e 3DNow! (`pfmul/pfadd`) num caminho alternativo de CPU;
  nenhuma foi alcançada até a tela título.
- 9 alvos de chamada não resolvidos (`recomp_stubs_unresolved.c`), nenhum alcançado.
- `AvSetDisplayMode`/PCRTC: `PCRTC_START` lê 0; a captura usa a superfície do `AvSetDisplayMode`.
