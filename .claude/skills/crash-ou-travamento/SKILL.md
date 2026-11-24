---
name: crash-ou-travamento
description: Diagnosticar quando o port do Digimon World 4 fecha sozinho, congela, fica em 0 fps ou não avança de uma tela. Use quando o usuário relatar crash, travamento, tela parada, ou quando um scripts/soak.py der CRASH, STALL ou INCOMPLETO.
---

# Crash, travamento ou progresso bloqueado

## 1. Ter um log

- Usuário jogando: `./run.sh > build/run.log 2>&1`, ou pelo launcher (logs em `logs/`, ou em
  `~/.local/share/dw4-recomp/logs/` num release). Em travamento, peça para esperar ~10 s antes
  de fechar: após 5 s sem quadro o log grava `[STALL]` com todas as threads.
- Reproduzir sem janela: `scripts/soak.py --scenario <arquivo> --runs N`. Para falha
  intermitente, meça a frequência (N ≥ 8) antes e depois da correção. Nunca rode dois jogos ao
  mesmo tempo.

## 2. Triagem

```bash
scripts/triage.py <log>
```

Nunca leia o log inteiro (é enorme): use o resumo e depois `grep -a -n` em volta da linha.

| Assinatura | Significado | Próximo passo |
|---|---|---|
| `[CRASH] SIG...` | falha fatal; traz registradores do guest, `sub_XXXXXXXX`, retornos do guest, últimos ICALLs, backtrace do host | achar a função em `src/recompiled/` pelo endereço |
| `[ICALL] ... Failed to resolve VA` | função só alcançada por ponteiro, não recompilada | semear (passo 3) |
| `[ICALL] alvo 0x00000000 ...` | ponteiro nulo/selvagem no guest; normalmente consequência de corrida ou estado errado | ver quem chama (`ret guest`) |
| `[STALL]` sem `retomou` | nenhum quadro apresentado; despejo de todas as threads | ver em que espera cada thread do guest está |
| `HalReturnToFirmware` | o próprio jogo desistiu | o erro verdadeiro está antes no log |
| `malloc(): ...` / abort da libc | corrupção de heap no host | `build-asan/` (`-DDW4_ASAN=ON`), ver `docs/REFERENCIAS.md` §4 |

Com o jogo vivo: `scripts/dump_threads.sh [--busy]` (despejo sob demanda, não mata o jogo).

## 3. Correções, em ordem de preferência

1. **Função faltando:** na pasta `xboxrecomp/`, `python -m tools.seed_from_log <log>` gera as
   sementes; grave em `config/seed_observed.json`, rode `scripts/regen.sh` e recompile. Cuidado
   com sementes falsas (valores com os 12 bits baixos zerados são constantes, não código).
2. **Falta no runtime (kernel, D3D, NV2A, áudio, USB):** primeiro procure uma variável
   `RECOMP_*` já existente no toolkit; os defaults do port ficam em `src/hooks/recomp_manual.c`.
   Se precisar mudar o toolkit, é patch (`patches/xboxrecomp-dw4.patch`, `PORT_NOTES.md` §4).
3. **Função traduzida errada:** override manual em `src/hooks/recomp_manual.c`
   (`recomp_lookup_manual`). Não edite `src/recompiled/`.

Antes de depurar do zero, leia `PORT_NOTES.md` §3 (bloqueios já resolvidos, com a causa de cada
um) e `docs/REFERENCIAS.md` §3–§5: muitos problemas são genéricos do toolkit e outros ports já
passaram por eles (trava de DPC, threads do guest uma por vez, fence do D3D, vblank).

Fatos do guest que costumam importar: `g_pDevice` em `0x002BAD98`; `D3D_BlockOnTime`
`0x002B1500`; o código do guest assume uma CPU, então todo ponto extra de preempção é uma
corrida em potencial.

Não use `RECOMP_PB_WALK=dma` nem desligue `RECOMP_FLIP_PACING` (causam crash/corrupção).

## 4. Fechar

- `scripts/soak.py --golden` e o roteiro que reproduzia a falha, várias vezes.
- Qualquer correção aqui fica no executável: **o usuário precisa reiniciar o jogo**.
- `PORT_NOTES.md` §3 (causa e correção) e `STATUS.md`. Se não achou a causa, registre a
  assinatura, a frequência e como reproduzir, e diga isso ao usuário sem amenizar.
