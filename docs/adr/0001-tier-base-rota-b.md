# ADR 0001: tier base pela rota B, sem interpretador

Data: 2026-07-27. Status: aceito.

## Decisao

O tier base do jit-v2 e um **template JIT (tier 1)** que carrega caches inline e
coleta perfil. Nao havera interpretador de bytecode. A desotimizacao tem como
alvo um **frame de tier 1 em um offset de bytecode conhecido**, o que exige um
mapa bytecode-para-codigo-de-maquina e um layout de frame estavel no tier 1.

## Alternativas consideradas

**Rota A: interpretador como tier 0.** Alvo de deopt trivial, perfil facil de
coletar, e um bonus de portabilidade para os backends POWER (um interpretador e
neutro de arquitetura).

**Rota B: template JIT como tier 1** (escolhida).

Nao existe rota C: sem um tier barato nao ha onde coletar perfil nem para onde
desotimizar.

## Criterio que decidiu

Nao foi o custo de escrever o interpretador, que e pequeno (7 opcodes de tres
enderecos, talvez 1200 linhas). Foram duas medicoes do codigo existente.

**1. As primitivas nao sao funcoes C.** Das 175 entradas de
`vm/runtime/Primitives.c:269`, **41 sao geradores de codigo de maquina** (`GEN`,
listadas em `vm/jit/PrimitivesGen.def`), e entre elas estao
`BlockValuePrimitive0..3`, `BlockValueArgsPrimitive`, `BlockWhileTruePrimitive`,
`BlockWhileTrue2Primitive`, `BlockOnExceptionPrimitive`,
`BlockUnwindPrimitive`, `ExceptionSignal` e `MethodSendPrimitive`. Essas
manipulam o frame nativo, a cadeia de unwind por fiber e os contextos
diretamente. A rota A exige uma segunda implementacao de cada uma, e uma segunda
semantica de unwind convivendo com a primeira na mesma pilha de fiber.

**2. O custo da rota A e duplicar o numero de motores de execucao que todo
subsistema transversal precisa entender.** Neste VM a lista e: raizes de GC
(`Scavenger.c:282`, que anda frames nativos por stackmap), unwind de excecao
(cadeia de `UnwindHandler` por fiber), reflexao de contexto (`thisContext`,
`Context>>parent/receiver/argumentAt:`, e `contextFrameOnCurrentStack` em
`StackFrame.c:121`, que valida um frame pelo span da pilha do fiber atual),
backtrace (`StackFrame.c:143`), safepoints com stop-the-world multicore, e
pinning de fiber.

Do outro lado, **o que a rota B exige a mais ja existe no repositorio**:

- mapa bytecode para codigo: `createBytecodeDescriptor` (`CodeDescriptors.h:67`)
  com busca por posicao nativa (`CodeDescriptors.h:141`), ja emitido em todo send
  (`CodeGeneratorX64.c:1562`) e ja guardado em `NativeCode.descriptors`. Mais:
  `generateBody` **ja computa `machineOffsetAt[]` por offset de bytecode**
  (`CodeGeneratorX64.c:384`) e o libera no fim (`:694`). O mapa da rota B e
  literalmente calculado hoje e descartado;
- frame canonico e identico em todo metodo framed (`CodeGeneratorX64.c:184-217`),
  com os acessores de `StackFrame.c:70-88` como contrato declarado e um
  `PORT_ME(frame-layout)` explicito;
- stackmap em todo send e todo `CCall` com `storeIp`, ou seja em todo ponto onde
  uma deopt precisaria reconstruir estado;
- `frameRawAreaSize` (`CodeGenerator.h:19`) ja participa do calculo do stackmap
  nos DOIS backends e vale zero: a area de frame para valores nao varridos ja
  esta furada.

A reescrita do zero reforca a decisao: a objecao usual a rota B e o custo de
retrofitar mapa e frame estavel num JIT que nao os tem. Nao ha retrofit.
Projetar o tier 1 com layout fixo e mapa por bytecode desde o inicio custa menos
que projeta-lo sem, porque o mesmo mapa entrega OSR, backtrace exato e o `bci`
que o `Context` reflexivo ja precisa.

## Custos aceitos

O layout de frame do tier 1 vira invariante: slot i = variavel de bytecode i,
sem promocao a registrador alem de um conjunto fixo. Isso deixa o tier 1 mais
lento que o tier 0 de hoje, cujo linear scan promove variaveis a registradores.
**Esse custo tem que ser medido e registrado, nao estimado.**

A rota A daria de graca um alvo de deopt neutro de arquitetura. A rota B nao.
Aceito: o alvo declarado e x86-64, e os backends POWER ja estao fora do gate
default (nada em `run_tests.sh` ou `build.sh` os invoca; rodam por
`scripts/ppc64/` sob QEMU, manualmente).

## O que invalidaria esta decisao

- Se o `--deopt-stress` da fase 3 nao fechar contra o tier 1 por uma razao
  estrutural, e nao por bug. Nesse caso a saida NAO e adotar a rota A, e sim um
  interpretador restrito usado SO como oraculo de verificacao diferencial, sem
  primitivas `GEN` e nunca em producao.
- Se o custo medido do frame fixo do tier 1 passar de cerca de 30% contra o
  tier 0 atual em Richards. Ai vale reconsiderar promocao limitada a
  registradores com um mapa de deopt por registrador, nao trocar de rota.
