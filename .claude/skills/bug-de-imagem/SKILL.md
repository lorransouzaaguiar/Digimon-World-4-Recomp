---
name: bug-de-imagem
description: Investigar e corrigir um defeito de imagem no port do Digimon World 4 (objeto invisível, cor errada, textura trocada, modelo com buracos, efeito parado). Use quando o usuário relatar algo que aparece errado na tela, mencionar F9 ou uma captura em build/shots.
---

# Defeito de imagem

Quem vê a tela é o usuário; você trabalha por capturas e pelo teste sem janela. Não abra a janela
do jogo sem avisar.

## 1. Obter a evidência

- Peça ao usuário para deixar o defeito na tela e apertar **F9** (janela do jogo em foco). Sai
  `build/shots/f9-NNN.bmp` e `f9-NNN.txt`: um frame A normal, um frame B sem os draws só de
  profundidade, pixels pintados por draw (`[Q]`), textura/alfa/UV (`[QX]`), sonda do pixel
  central (`[PROBE]`).
- Defeito que dura um quadro ("a tela pisca"): a caixa-preta grava sozinha os últimos ~2 s em
  `build/shots/flash-NNN/` quando detecta um clarão (linha `[FLASH]` no log), ou com **F12**.
  Veja `info.txt` e o quadro marcado SUSPEITO (`PORT_NOTES.md` §7.7).
- Se der para chegar à cena por roteiro, reproduza sem janela: cenário em `tests/scenarios/`
  com `shot:<nome>` e `scripts/soak.py --scenario <arquivo>`; `-e DW4_GL_F9_AT=<segundos>` dispara
  a captura F9 sem teclado.
- Teclas que o usuário pode testar ao vivo e relatar: F5 culling, F6 alpha test, F7 teste de
  profundidade, F8 combiners, F10 pula draws só de profundidade, F11 ignora z-bias. Qual tecla
  "conserta" já aponta a área.

## 2. Ler a captura

```bash
scripts/f9_diff.py                 # captura mais recente: o que está escondido e por quem
```

Converta o BMP para PNG (ffmpeg) e olhe. No `.txt`, agrupe os draws da região do defeito por
textura e estado (`cmask`, `ztest`, `zwrite`, `blend`, `atest`, `cull`, `tex=`).

Regras de leitura: um acerto é evidência, uma ausência não; leia totais, não linhas impressas.

## 3. Renderizador ou executor?

Compare com o rasterizador em CPU do toolkit: `scripts/soak.py --sw --scenario <arquivo>`.

- **CPU certo, GPU errado** → defeito no renderizador: `src/hooks/nv2a_gl_backend.c` (OpenGL) ou
  `src/hooks/nv2a_vk_backend.c` + `src/hooks/shaders/` (Vulkan). Veja qual estava em uso; se o
  defeito for na lógica de combiners/estado, corrija nos dois.
- **Os dois errados** → defeito antes do renderizador: executor de pushbuffer do toolkit
  (`xboxrecomp/src/kernel/nv2a_pb_exec.c`) ou estado que ele não repassa. Foi o caso da matriz de
  textura (ponte invisível, transição cinza) e do giro das tiras de triângulos.

Antes de inventar, veja `PORT_NOTES.md` §7 (casos resolvidos e o método) e a tabela de correções
dos outros ports em `docs/REFERENCIAS.md` §3. Texgen: só EYE_LINEAR e OBJECT_LINEAR (`PORT_NOTES.md` §4.8); sphere, reflection e normal map não.

## 4. Corrigir e entregar

- **No renderizador:** `cmake --build build --target dw4_gl`; o jogo aberto recarrega sozinho
  (procure `[GL] renderizador RECARREGADO` no log). Hotkeys voltam ao padrão na recarga.
- **No executor/toolkit ou no executável:** build completo e **o usuário precisa reiniciar o
  jogo**. Atualize `patches/xboxrecomp-dw4.patch` e `PORT_NOTES.md` §4.
- Diga claramente qual dos dois casos é.

## 5. Conferir

- `scripts/soak.py --golden`. Se as referências mudaram, olhe lado a lado antes de adotar
  (`scripts/golden.py build/soak/latest/01/shots --update`): uma "diferença" pode ser a correção
  de algo que estava errado na referência.
- O que só o usuário pode confirmar, peça para ele confirmar; registre no `STATUS.md` como
  "aguardando confirmação" até lá.
- Documente causa e método em `PORT_NOTES.md` §7, inclusive as hipóteses descartadas.
