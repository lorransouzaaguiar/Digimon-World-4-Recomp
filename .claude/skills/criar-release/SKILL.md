---
name: criar-release
description: Empacotar e publicar uma nova versão do Digimon World 4 Recomp no GitHub (AppImage, tar.gz, checksums, notas em pt-BR e en-US). Use quando o usuário pedir um release, uma nova versão, ou para gerar o pacote/executável.
---

# Criar um release

Publicar é ação externa: só faça quando o usuário pedir, e confirme a versão. Gerar o pacote em
`dist/` é local e pode ser feito à vontade.

## 1. Antes

- Árvore limpa e `scripts/soak.py --golden` passando.
- `STATUS.md` e os dois READMEs refletem o que mudou (problemas conhecidos, requisitos).
- Versão `vX.Y.Z`: a anterior está em `git tag` e em `docs/releases/`.

## 2. Notas bilíngues

Crie `docs/releases/vX.Y.Z.md` seguindo `docs/releases/v0.1.0.md`: links no topo, seção
**English** e seção **Português (Brasil)** com o mesmo conteúdo — como começar, requisitos, o que
mudou, problemas conhecidos, como relatar. Escreva só o que foi verificado; o que não foi testado
entra em "problemas conhecidos". Sempre o aviso de que os arquivos do jogo não vêm junto.

## 3. Pacote

```bash
DW4_URL=https://github.com/lorransouzaaguiar/Digimon-World-4-Recomp scripts/package_release.sh vX.Y.Z
```

Gera em `dist/`: `DigimonWorld4-Recomp-vX.Y.Z-x86_64.AppImage`,
`...-linux-x86_64.tar.gz` e `SHA256SUMS-vX.Y.Z.txt`. O script baixa as ferramentas para `tools/`
na primeira vez e **falha de propósito** se achar arquivo do jogo, caminho da máquina de build ou
biblioteca que peça glibc acima de 2.38. Não contorne essas barreiras: corrija a causa.

## 4. Conferir o pacote fora do projeto

Em uma pasta do scratchpad, com `XDG_DATA_HOME` apontando para lá (não toque nos saves reais):

```bash
env -u LD_LIBRARY_PATH XDG_DATA_HOME=<tmp>/xdg ./DigimonWorld4-Recomp-*.AppImage --selftest
# jogo sem janela até a tela título, com game_data ligado por symlink em <tmp>/xdg/dw4-recomp/:
env -u LD_LIBRARY_PATH XDG_DATA_HOME=<tmp>/xdg RECOMP_HEADLESS=1 SDL_AUDIODRIVER=dummy \
    DW4_INPUT_SCRIPT=@<roteiro> ./DigimonWorld4-Recomp-*.AppImage --play
```

Confira no log: `Config:` com a versão certa, `[GL] back end OpenGL`, renderizador carregado de
dentro do pacote, saves na pasta de teste. Janela, som, controle e imagem de disco real só o
usuário confirma: diga isso.

## 5. Publicar (após o usuário confirmar)

```bash
git add -A && git commit        # notas + docs
git tag -a vX.Y.Z -m "Digimon World 4 - Recomp vX.Y.Z"
git push origin main && git push origin vX.Y.Z
```

Não há `gh` nesta máquina; o release é criado pela API do GitHub com a credencial que o git já
guarda. Nunca imprima o token.

```bash
TOKEN=$(printf 'protocol=https\nhost=github.com\n\n' | git credential fill | sed -n 's/^password=//p')
R=lorransouzaaguiar/Digimon-World-4-Recomp
# corpo: JSON gerado em Python com open("docs/releases/vX.Y.Z.md").read()
curl -s -X POST -H "Authorization: Bearer $TOKEN" https://api.github.com/repos/$R/releases --data-binary @rel.json
# anexos: POST https://uploads.github.com/repos/$R/releases/<id>/assets?name=<arquivo>  (application/octet-stream)
```

Leia as respostas com `python3 -c "json.load(sys.stdin)"` direto do `curl` (no zsh, `echo` de uma
variável estraga os `\n` do JSON). Se `gh` estiver instalado, `gh release create vX.Y.Z
--notes-file docs/releases/vX.Y.Z.md dist/<arquivos>` faz o mesmo.

## 6. Depois

- Liste o release pela API e compare tamanho e `digest` de cada anexo com `SHA256SUMS`.
- Informe ao usuário o link, se saiu como rascunho/pré-lançamento ou publicado, e o que não foi
  testado.
- Atualize o "Estado" no `CLAUDE.md` e o `STATUS.md`.
