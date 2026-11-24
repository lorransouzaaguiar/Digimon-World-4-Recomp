# Digimon World 4 — Recomp

[English](README.md) · **Português (Brasil)**

**Recompilação estática experimental de Digimon World 4 (Xbox original, 2005)** em um programa
nativo x86-64 (Linux hoje; Windows e macOS estão planejados), feito com o [xboxrecomp](https://github.com/sp00nznet/xboxrecomp).
O código do próprio jogo é traduzido para C uma vez e compilado nativamente; o runtime do
xboxrecomp e o código deste projeto substituem o kernel, a GPU, o áudio e a entrada do Xbox.
Não é um emulador.

> Este repositório **não contém código nem dados do jogo**. Você precisa da sua própria cópia,
> obtida legalmente, de Digimon World 4 para Xbox (NTSC-U). O código C é gerado na sua máquina a
> partir do seu `default.xbe` e nunca é versionado.

![Tela título](docs/title_screen.png)

## Estado

Jogável do boot até a primeira fase a 60 fps na máquina de teste (CachyOS, NVIDIA).
Inicial e experimental: espere bugs.

| Marco | |
|---|---|
| Pipeline: 23.472 funções traduzidas, 0 erros de tradução, 2,25 M de linhas de C | ✅ |
| Host nativo Linux (SDL2, threads POSIX), acesso aos arquivos do jogo sem diferenciar maiúsculas | ✅ |
| Boot, vídeos de abertura (CRI Sofdec), tela título | ✅ |
| Menus, criação do Digimon, salvar e carregar | ✅ |
| Renderizador OpenGL 3.3 com cache de texturas: 60 fps no lobby (o de CPU faz ~3 fps e fica como referência) | ✅ |
| Controle (padrão Xbox/XInput) e teclado | ✅ |
| Lobby, tutorial e primeira fase jogáveis | ✅ |
| Fases seguintes, chefes, o resto do jogo | ⬜ ainda não testado |
| Multijogador (2 a 4 jogadores) | ⬜ ainda não testado |
| Resolução interna maior, widescreen, remapear teclas | ⬜ não implementado |
| Renderizador Vulkan | 🔧 em desenvolvimento |
| Windows e macOS | ⬜ planejado, não iniciado |
| Steam Deck | ⬜ não testado |

Detalhes e a lista do que falta: [STATUS.md](STATUS.md).

### Problemas conhecidos

- Pular o vídeo de abertura com START às vezes fecha o jogo (cerca de 1 em cada 4 execuções nos
  testes automáticos). Abra de novo; deixar o vídeo tocar evita o problema.
- A geração automática de coordenadas de textura (reflexos, brilhos) não está implementada; alguns
  objetos podem aparecer chapados.
- Testado em uma única máquina. Outras GPUs, drivers e distribuições não foram testados.

## Jogar a partir de um release

Um download só, sem instalar nada:

1. Baixe `DigimonWorld4-Recomp-<versão>-x86_64.AppImage` na [página de releases](https://github.com/lorransouzaaguiar/Digimon-World-4-Recomp/releases).
2. Torne o arquivo executável e abra: `chmod +x DigimonWorld4-Recomp-*.AppImage`, depois dois
   cliques (ou `./DigimonWorld4-Recomp-*.AppImage`).
3. Na tela **Jogar**, escolha a imagem (`.iso`) do seu próprio disco de Digimon World 4, ou uma
   pasta com o disco já extraído. O launcher copia os quatro arquivos de que precisa.
4. Clique em **JOGAR**.

O AppImage traz o launcher (com o Qt), o jogo recompilado e as bibliotecas necessárias (SDL2,
libepoxy, libcrypto). Ele **não** traz os dados do jogo: esses vêm do seu disco.

Requisitos: Linux x86-64 com glibc 2.38 ou mais nova (Ubuntu 24.04+, Fedora 39+, Arch, SteamOS
3.6+) e placa de vídeo com driver OpenGL 3.3 funcionando. Se o AppImage não abrir por falta de
FUSE, rode com `--appimage-extract-and-run` ou use o pacote `.tar.gz` (mesmo conteúdo: extraia e
rode `./DigimonWorld4-Recomp`).

Saves, opções e logs ficam em `~/.local/share/dw4-recomp/`, fora do AppImage; trocar o arquivo por
uma versão nova não mexe neles.

## Launcher

```bash
./launcher.sh            # abre o launcher
./launcher.sh --play     # inicia o jogo direto, com as opções salvas
```

![Launcher](docs/launcher.png)

| Tela | O que faz |
|---|---|
| Jogar | arquivos do jogo: escolha a imagem do disco (`.iso`) ou uma pasta; placa de vídeo detectada, versão, botão Jogar |
| Gráficos | tela cheia, tamanho da janela (1× a 4× de 640×480), sincronização vertical, renderizador GPU ou CPU |
| Controles | teclado como controle 1 ligado/desligado, tabela de teclas |
| Saves e dados | pasta dos saves, lista de saves, backup, exportar e importar `.zip` (os saves atuais são guardados antes) |
| Suporte | problemas conhecidos, pasta de logs, relatório de problema sem o nome da sua pasta pessoal |

A interface é em português (Brasil) ou inglês, conforme o idioma do sistema; dá para trocar na
barra lateral. Toda execução pelo launcher é gravada em `logs/` (ficam as 10 últimas). As opções
ficam em `config/launcher.json`. No código-fonte essas pastas ficam no projeto; num release, em
`~/.local/share/dw4-recomp/`.

Os releases já trazem tudo de que o launcher precisa. No código-fonte ele usa o PySide6 (Qt para
Python), que o `scripts/setup.sh` instala no `.venv`; o `./run.sh` abre o jogo sem ele.
A divisão de telas segue a do launcher do [hayatog/iss_pc](https://github.com/hayatog/iss_pc); o
código é deste projeto.

## Arquivos do jogo

Você precisa do seu próprio disco de Digimon World 4 para Xbox, **NTSC-U** (title ID `42410002`).
Outras regiões não foram testadas. O launcher lê a imagem do disco diretamente (XISO só da partição
do jogo ou imagem completa estilo Redump) e também aceita uma pasta com o disco extraído. Os
arquivos necessários ficam na raiz do disco:

```
default.xbe   area.afs   ndmw.afs   ndmw.igb
```

Nunca compartilhe arquivos do jogo, nem os anexe a relatos de bug.

## Compilar a partir do código

Arch / CachyOS:

```bash
sudo pacman -S --needed base-devel cmake ninja clang python sdl2 libepoxy mesa openssl
```

```bash
git clone --recursive https://github.com/lorransouzaaguiar/Digimon-World-4-Recomp.git
cd Digimon-World-4-Recomp
scripts/setup.sh                 # submódulo do toolkit, patches locais, ambiente Python

# copie o conteúdo do seu disco para game_data/

scripts/regen.sh                 # gera src/recompiled/ a partir do seu default.xbe (~3 min)
export PATH=$PWD/.venv/bin:$PATH
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build -j$(nproc)
./run.sh
```

Compilar o código gerado pede vários GB de RAM; diminua o `-j` se faltar memória.

### Pacote de release

```bash
scripts/package_release.sh v0.1.0
```

Gera em `dist/` o `.AppImage`, um `.tar.gz` com o mesmo conteúdo e um arquivo `SHA256SUMS`. O
script baixa as ferramentas de empacotamento para `tools/` (appimagetool e um Python portátil usado
só para congelar o launcher) e se recusa a terminar se o pacote tiver arquivo do jogo, caminho da
máquina de build ou biblioteca que peça glibc mais nova que a 2.38.
O executável do pacote contém o código recompilado do jogo; os dados do disco nunca vão junto.

## Controles

Qualquer controle que o sistema reconheça como Xbox/XInput funciona. O teclado age como o
controle 1:

| Tecla | Controle do Xbox |
|---|---|
| Enter | START |
| Backspace | BACK |
| Z / X / C / V | A / B / X / Y |
| Setas | Direcional |
| W A S D | Analógico esquerdo |
| I J K L | Analógico direito |
| Q / E | White / Black |
| 1 / 3 | Gatilho esquerdo / direito |
| Shift / Ctrl | Clique do analógico esquerdo / direito |

## Relatar um problema

Rode guardando o log e anexe o arquivo:

```bash
./run.sh > run.log 2>&1
```

- **Crash:** o log termina com um relatório `[CRASH]`.
- **Travamento:** espere uns dez segundos antes de fechar; depois de 5 s sem quadro novo o log
  registra onde cada thread parou (`[STALL]`).
- **Defeito de imagem:** aperte **F9** com o defeito na tela (builds a partir do código). Ele grava
  uma captura e a lista de todos os draws em `build/shots/`.

`DW4_SW_RENDER=1 ./run.sh` usa o renderizador de referência em CPU, para separar bug do
renderizador de bug do jogo.

## Para quem desenvolve

- [STATUS.md](STATUS.md): o que funciona, o que falta.
- [PORT_NOTES.md](PORT_NOTES.md): endereços, hooks, patches no toolkit, como cada bug foi achado.
- [docs/REFERENCIAS.md](docs/REFERENCIAS.md): o que outros ports com o mesmo toolkit fazem.
- `scripts/soak.py` roda o jogo sem janela a partir de um roteiro de entrada e classifica o
  resultado; `scripts/golden.py` compara quadros com referências; `scripts/triage.py` resume um log.
- O renderizador (`src/hooks/nv2a_gl_backend.c`) recarrega a quente: rode
  `cmake --build build --target dw4_gl` com o jogo aberto.
- As mudanças no toolkit ficam em `patches/xboxrecomp-dw4.patch` e são aplicadas por
  `scripts/setup.sh`.

## Como funciona

A CPU do Xbox é um x86, então o código de máquina do jogo é traduzido função por função para C e
compilado para x86-64. O jogo conversa com o kernel do Xbox por 128 chamadas importadas, que o
toolkit implementa no Linux. Ele comanda o chip gráfico (NV2A) escrevendo buffers de comandos
diretamente; o toolkit decodifica esses comandos e o back end OpenGL deste projeto os desenha, com
os register combiners convertidos em shader e as texturas guardadas em cache por endereço e
conteúdo.

## Créditos

- [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp): o recompilador e o runtime sobre
  os quais este port é feito.
- Outros ports com o mesmo toolkit, estudados como referência de ferramentas, testes e
  empacotamento (nenhum código copiado):
  [Amped2-Recomp](https://github.com/bertibertig/Amped2-Recomp),
  [SSX3-Recomp](https://github.com/bertibertig/SSX3-Recomp),
  [SSX3-recomp](https://github.com/koenhendriks/SSX3-recomp),
  [DefJamFFNY-recomp](https://github.com/vyanhursky/DefJamFFNY-recomp),
  [blinx2-recomp](https://github.com/apexJCL/blinx2-recomp).

## Aviso legal

Projeto de fãs, não oficial, sem vínculo com a Bandai Namco, a Toei Animation ou a Microsoft nem
endosso delas. Digimon World 4 e todos os nomes e artes relacionados pertencem aos seus
detentores. As capturas de tela estão aqui para documentar o port; a licença do projeto não as
cobre.

## Licença

A definir antes de o projeto se tornar público. O toolkit xboxrecomp é MIT, com partes LGPL-2.1
derivadas do xemu.
