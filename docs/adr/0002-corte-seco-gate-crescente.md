# ADR 0002: corte seco com gate crescente

Data: 2026-07-27. Status: aceito.

## Decisao

`vm/jit/` e `vm/memory/` sao **removidos inteiros** da branch `jit-v2`, junto com
`vm/compiler/Optimizer.c`. O JIT e o GC sao reimplementados do zero. Nao ha
convivencia entre v1 e v2, nem flag `--jit=v1|v2`.

Como consequencia, `run_tests.sh` **nao fica verde a cada commit**. O contrato de
corretude passa a ser um **gate crescente**, definido em
`docs/jit-v2/01-gate.md`: uma escada de niveis, cada um com criterio objetivo,
que so avanca e nunca regride. Um commit e valido se o gate do seu nivel passa.

## Alternativas consideradas

**v1 como regua, apagado no fim.** v2 nasce ao lado no mesmo binario, a suite
continua verde pelo v1 enquanto v2 cresce, e v1 e deletado no commit em que v2
passa em tudo. v2 nao compartilharia uma linha com v1, entao nao herdaria
defeito; v1 seria oraculo diferencial, nao fundacao.

**JIT primeiro, GC depois.** Reescrever o JIT contra o GC atual (com uma mudanca
contida para o scavenger honrar a area de frame crua, cuja aritmetica ja existe),
suite verde o tempo todo; depois reescrever o GC contra o JIT novo. Duas
travessias, nenhuma cega.

**Corte seco com gate crescente** (escolhida).

## Criterio que decidiu

Decisao do dono do projeto, tomada com o custo na mesa. As duas alternativas
mantinham a suite verde; o corte seco nao. A escolha foi por nao carregar o
codigo antigo nem as suas premissas, ao preco de perder o oraculo diferencial.

## Risco aceito, e o que fazemos a respeito

O risco especifico, e ele e concreto neste VM: **um slot de stackmap faltando NAO
produz teste vermelho**. O scavenger atual reescreve para `nil`, em silencio,
todo slot marcado como raiz cujo conteudo nao passa em `plausibleObject`
(`Scavenger.c:239-279`), porque o alocador linear-scan e cego a fluxo de controle
e marca slots que ainda contem lixo. Sem oraculo, essa classe de bug fica
invisivel ate virar corrupcao.

Tres mitigacoes, que sao obrigacoes de projeto e nao boas intencoes:

1. **O GC novo nao repara.** Um slot que o mapa diz ser ponteiro TEM que ser
   ponteiro; divergencia e `ASSERT`, nunca reparo. Isso so e possivel porque o
   mapa de v2 sai de liveness sobre SSA, e nao de um linear scan cego a fluxo.
   Registrado como requisito em ADR 0003.
2. **`--deopt-stress` e o oraculo interno** (fase 3): com todo guard falhando, o
   resultado de todo teste e todo benchmark tem que ser identico ao do caminho
   sem stress. Isso substitui o oraculo externo que o corte seco tirou, e por
   isso a fase 3 nao pode ser adiada.
3. **Os numeros de referencia do v1 sao capturados ANTES do corte** (fase 1). Sem
   eles, "zero alocacoes" nao tem denominador e "mais rapido" nao tem contra o
   que.

## O que invalidaria esta decisao

- Se o gate travar em um nivel por mais de uma semana de trabalho sem progresso
  mensuravel, o diagnostico provavel e falta de oraculo, e a resposta e voltar
  atras e reintroduzir um: `git worktree` com master em paralelo, executando os
  mesmos programas e comparando saidas. Isso nao ressuscita o v1 na branch, so o
  usa como referencia externa.
- Se aparecer uma classe de bug que o `--deopt-stress` provadamente nao alcanca.
