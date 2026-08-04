# Escopo revisado

Este documento substitui a secao 9 de `00-estado-atual.md`, que foi escrita
antes do corte e antes das decisoes que vieram depois. O escopo cresceu duas
vezes desde entao, e o que valia no inicio nao vale mais.

## A regra

**Reimplementacao do analisador semantico em diante.** Nao ha retro-
compatibilidade a preservar: nenhuma camada de adaptacao, nenhuma API mantida
so para nao mexer em chamadores. Quando o modelo novo discorda do antigo, o
chamador muda.

A UNICA obrigacao externa e continuar gerando as imagens dos pacotes: o VM tem
que ler `packages/Core` e produzir uma imagem carregavel. Como `run_tests.sh` e
`run_benchmarks.sh` regeram essa imagem a cada execucao, a imagem e DERIVADA e
nao ha migracao de formato a fazer. Isso e o que torna a liberdade acima
possivel.

## Fica

| | |
|---|---|
| `vm/compiler/Tokenizer.c` | lexico puro, 541 linhas |
| `vm/compiler/Parser.c` | sintaxe pura, 1298 linhas |
| `vm/compiler/Ast.h` | a arvore, 619 linhas |
| `vm/os/` | camada de SO, ja isolada atras de `Os.h` |
| `packages/`, `samples/`, `tests/`, `benchmarks/` | codigo Smalltalk |
| `run_tests.sh`, `run_benchmarks.sh`, `scripts/` | ferramental |

Fronteira: **sintaxe fica, semantica em diante e nova.** O parser produz uma
arvore; tudo o que se faz com ela e novo.

## Reimplementa

| | por que |
|---|---|
| analise semantica (`Scope.c`) | resolucao de nomes precisa carregar tipo, nao so escopo |
| geracao de bytecode (`Compiler.c`) | bytecode novo, ver abaixo |
| **conjunto de bytecodes** | registradores, tres enderecos, desenhado para SSA e para deopt |
| **sistema de tipos** | ver ADR 0006 |
| **fibers e scheduler** | prioridade alta, ver abaixo |
| **primitivas** | as 175, provavelmente todas |
| **JIT inteiro** | tiers, IC, deopt, SSA, passes, codegen |
| **GC e modelo de objeto** | FEITO, gate nivel 0 verde |
| `Snapshot.c` | serializa o modelo de objeto, que mudou |
| `Bootstrap.c` | constroi as classes do kernel com as formas novas |

## Ordem

O gate (`01-gate.md`) diz para onde, esta secao diz por onde.

1. **modelo de objeto e GC** FEITO, nivel 0 verde, 17 de 17
2. **fibers**, porque sao a base sobre a qual frames nativos, safepoints,
   unwind e o scheduler todo se apoiam, e porque errar a troca de contexto
   depois de existir um JIT em cima e caro de diagnosticar
3. **bytecode novo + analise semantica + tipos**, que juntos definem a entrada
   do JIT
4. **tier 1 template JIT** com frame fixo, mapa bytecode para maquina e IC
5. **deopt**, antes do otimizador, sem excecao
6. **SSA, passes, codegen**
7. **classes de valor e arrays planos**

Fibers subiram para o segundo lugar a pedido explicito. A justificativa
tecnica coincide: o contrato de raizes (`memory/Roots.h`) ja esta escrito
esperando frames nativos, e quem os cria e destroi e a troca de fiber.

## O que isso significa para o que ja existe

Nada do que foi escrito ate aqui vira lixo: o modelo de objeto, a tabela de
classes, o coletor e o contrato de raizes sao a fundacao dos itens 2 a 7 e nao
dependem de nenhum deles. O que muda e que **nao existe mais a lista de "fica,
mas tem que acompanhar"**: os arquivos daquela lista ou ficam intocados
(sintaxe, SO, Smalltalk) ou sao reescritos.

Em particular, os 177 usos de `->class` que a mudanca de cabecalho quebrou
estao, na maioria, em arquivos que vao ser reescritos de qualquer forma:
`Primitives.c` (32), `Compiler.c` (26), `Dictionary.c` (19), `Scope.c` (12),
`Snapshot.c` (9), `Bootstrap.c` (7). Consertar cada um deles agora, no modelo
antigo, seria trabalho jogado fora duas vezes.
