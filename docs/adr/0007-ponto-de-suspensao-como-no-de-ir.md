# ADR 0007: o ponto de suspensao e um no de IR, unificado com o estado de deopt

Data: 2026-07-27. Status: aceito.

## Decisao

A IR ganha **dois nos**, e nao um conjunto de instrucoes de fiber:

- `safepoint` — a execucao PODE sair daqui (um coletor pediu o mundo parado).
  Carrega um mapa de estado exato. Emitido em back-edge de laco e na entrada de
  metodo framed.
- `suspend` — a execucao VAI sair daqui e voltar depois (yield, espera de canal,
  espera de semaforo). Carrega o mesmo mapa de estado, mais o bci de retomada.

Os dois usam **a mesma estrutura de estado que o guard de desotimizacao usa**:
pilha de frames virtuais, cada um com metodo, offset de bytecode e mapeamento
de slot para valor, com os slots tipados (`memory/Roots.h`: ponteiro, f64 cru,
i64 cru, morto).

O bytecode ganha **um** opcode, o ponto de suspensao, para que o frame de
tier 1 e o mapa bytecode-para-maquina o cubram e o OSR funcione dentro de um
laco que contem um yield.

FORA da IR, por decisao e nao por esquecimento: `spawn`, criacao de canal, envio
para actor, `Semaphore>>wait`, e toda politica de escalonamento. Sao sends com
cache inline como qualquer outra coisa.

## Criterio que decidiu

**Um no de IR so se justifica se algum passe toma uma decisao diferente por
causa dele.** Para `spawn` e companhia nenhum passe toma: sao chamadas opacas e
o otimizador ja sabe tratar chamadas opacas. Para o ponto de suspensao varios
tomam: a liveness precisa dele, o mapa de estado precisa dele, e LICM e GVN
precisam trata-lo como barreira.

E ha uma unificacao que o torna quase gratuito. Estas tres coisas sao
estruturalmente a MESMA:

| | o que e | o que exige |
|---|---|---|
| guard que falha | sai do codigo otimizado, nao volta | estado abstrato reconstruivel |
| safepoint de GC | pausa, e um peer varre esta pilha | estado abstrato descrito com precisao |
| yield de fiber | sai do frame e volta depois | estado abstrato descrito com precisao |

Todas sao "um ponto onde a execucao pode deixar o codigo otimizado e o estado
abstrato tem que estar descrito". UM mecanismo entrega as tres. E a ordem de
fases ja imposta ao projeto (infra de desotimizacao ANTES do otimizador) passa
a construir o suporte a fiber de graca.

## O ganho, e ele e medivel

O VM antigo emitia isto em toda back-edge (`CodeGeneratorX64.c:286-319`,
citado em `docs/jit-v2/00-estado-atual.md`):

> `atBackEdge` additionally OVER-APPROXIMATES that stackmap: the linear-scan
> allocator's [start,end] liveness is control-flow-unaware and omits a
> loop-carried pointer whose last textual use precedes the back-edge though it
> is live into the next iteration.

Ou seja: o poll de safepoint marcava **todo temp spillado** como raiz, em todo
laco do sistema, porque a analise de liveness nao sabia de fluxo de controle. A
imprecisao existia precisamente porque o poll NAO fazia parte da IR.

Com o ponto de suspensao como no de SSA, a liveness e a mesma do resto do
otimizador e a over-aproximacao deixa de ser necessaria. Some uma classe de
imprecisao, e e a mesma que obrigava o coletor antigo a tolerar slots obsoletos,
que e a mesma que justificava o reparo silencioso que o ADR 0003 R2 proibe.

**O criterio de verificacao**: quando o codegen novo existir, nenhum caminho
pode ter um equivalente de `overapproxStackmap`. Se um aparecer, esta decisao
nao foi implementada, so nomeada.

## Alternativas consideradas

**Nenhum no, tudo por send e primitiva**, como o VM antigo. O custo e o acima:
o otimizador nao sabe o que um poll faz, entao ou ele e uma barreira total ou o
mapa e over-aproximado. O VM antigo escolheu over-aproximar.

**Um conjunto de instrucoes de fiber** (`spawn`, `yield`, `send`, `receive`).
Poe politica de biblioteca na IR, acopla o JIT ao modelo de concorrencia, e
nenhum passe usaria a maioria delas.

## O risco, e a mitigacao

O risco e acoplar o JIT ao modelo de concorrencia antes de o modelo estar
decidido (fibers pinados contra migraveis, work-stealing, outro scheduler).

A mitigacao esta no nome e no conteudo do no: ele e **abstrato**, `suspend` com
um mapa de estado, e nao concreto, `fiberYield`. Se o modelo mudar, o no nao
muda: continua sendo "aqui a execucao pode sair e o estado tem que estar
descrito". O que muda e quem o emite e o que acontece depois dele.

## O que invalidaria esta decisao

- Se medirmos que tratar `suspend` como barreira de movimentacao custa mais do
  que a over-aproximacao custava. Improvavel: uma barreira em back-edge impede
  mover codigo PARA FORA do laco atraves dela, e LICM ja precisa provar
  invariancia de qualquer forma.
- Se o modelo de concorrencia passar a ter suspensao em pontos que o compilador
  nao consegue enumerar (preempcao real por sinal, em qualquer instrucao). Ai o
  mapa exato deixa de ser construivel e o desenho volta a precisar de
  conservadorismo em algum lugar.
