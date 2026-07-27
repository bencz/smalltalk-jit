# jit-v2, fase 0: estado atual

Reconhecimento do que existe hoje, antes de tocar em qualquer coisa. Toda
afirmacao aqui cita arquivo e linha. Nenhuma linha de codigo de JIT foi escrita
nesta fase.

Base medida no commit `583047d` (master), branch `jit-v2` criada a partir dele:

- `./run_tests.sh --no-build`: **158 ok, 0 falhas, 3375 ms**
- build limpa com `-Werror`, sem avisos

O prototipo de referencia (`python_proto/`) foi executado: **27 de 27
verificacoes passam**, 27,3x de tier 0 para tier 2, resultado `149876875.0`. O
corpo do laco interno depois da otimizacao, que e a especificacao de aceitacao
da fase 6, sai assim:

```
v40:bool = icmp {<=} (v28, v25)      <- condicao do laco
v43:void = guard_class {14} (v42)    <- unico guard
v45:f64 = vaload {0} (v42, v28)      <- tres cargas diretas do array plano
v46:f64 = vaload {1} (v42, v28)
v47:f64 = vaload {2} (v42, v28)
v54:f64 = fmul (v45, v129)           <- seis mulsd (scale: e dot:)
v60:f64 = fmul (v46, v129)
v66:f64 = fmul (v47, v129)
v80:f64 = fmul (v45, v54)
v89:f64 = fmul (v46, v60)
v93:f64 = fadd (v80, v89)            <- tres addsd
v102:f64 = fmul (v47, v66)
v106:f64 = fadd (v93, v102)
v112:f64 = fadd (v32, v106)          <- acumulador f64 (phi promovido)
v117:i64 = iadd (v28, v134)          <- contador i64 (phi promovido)
```

Passes que produziram isso, em uma compilacao de 2,3 ms: 7 guards removidos, 11
escalarizados, 19 por GVN, 21 pares de box, **4 phis promovidos**, 9 hoisted,
18 mortos.

**Decisoes tomadas apos esta fase**, cada uma com ADR:

- ADR 0001: tier base pela **rota B**, template JIT, sem interpretador.
- ADR 0002: **corte seco** com gate crescente, sem convivencia v1/v2.
  Contrato de corretude em `docs/jit-v2/01-gate.md`.
- ADR 0003: GC **reprojetado**, com requisitos R1 a R9 fixados e a escolha do
  algoritmo adiada para depois da fase 1, por criterio explicito.

Tamanho do que esta em jogo, em linhas de `.c`/`.h`/`.def`:

| diretorio | linhas | destino no plano novo |
|---|---:|---|
| `vm/jit/` (comum) | 3493 | REESCREVER |
| `vm/jit/x64/` | 6592 | REESCREVER |
| `vm/jit/ppc64/` | 6181 | ver secao 10 |
| `vm/memory/` | 3544 | REESCREVER |
| `vm/compiler/` | 5898 | MANTER (menos `Optimizer.c`) |
| `vm/core/` | 4193 | MANTER em parte, ver secao 9 |
| `vm/runtime/` | 6670 | MANTER, com adaptacoes |
| `vm/concurrency/` | 1605 | MANTER, com adaptacoes |
| `vm/tools/` | 1600 | MANTER (`Snapshot.c` precisa acompanhar o modelo de objeto) |
| total `vm/` | 47609 | |

---

## 1. Esquema de tagging

**2 bits de tag, 4 tags, todas as 4 em uso.** `vm/core/Object.h:44-49`:

```c
typedef enum {
	VALUE_INT = 0,      // 00: SmallInteger, 62-bit payload
	VALUE_POINTER = 1,  // 01: heap object (asObject subtracts the tag)
	VALUE_CHAR = 2,     // 10: Character
	VALUE_FLOAT = 3,    // 11: SmallFloat64, immediate double
} ValueType;
```

- `Value` e `uintptr_t`, com `_Static_assert` de 64 bits em `Object.h:57`. Alvos
  de 32 bits estao fora de escopo por design.
- **SmallInteger**: tag `00`, payload SINALIZADO de 62 bits, faixa
  `[-2^61, 2^61-1]`, ASSIMETRICA. `tagInt` em `Object.h:307-319`, `asCInt` em
  `Object.h:284-289` (`(SignedValue) value >> 2`).
- **Float**: existem DUAS representacoes.
  1. **SmallFloat64 imediato**, tag `11`, ja implementado, rotacao estilo Spur
     com payload de 62 bits: `Object.h:342-407`. `ROL64(bits,1)` alinha o double
     como `[expoente:11 | mantissa:52 | sinal:1]` e subtrai `768 << 53`. Cabem
     exatamente os doubles com expoente polarizado 768..1279, ou seja magnitude
     em `2^[-255, 256]`. `+-0.0` e caso especial (payloads 0 e 1). Subnormais,
     infinitos, NaN e magnitudes fora da janela ficam BOXED.
  2. **BoxedFloat64**, objeto de heap com um `double` no corpo:
     `RawFloat` em `Object.h:136-140`, `FloatShape` em `Object.h:163`.
- **Character**: tag `10`, byte normalizado por `unsigned char` (`Object.h:322-327`).
- **Ponteiro**: tag `01`, `asObject` subtrai 1 (`Object.h:300-305`).

Consequencia direta para o alvo: **o guard de aritmetica inteira ja e um `or`
seguido de um `test`**, so que contra a mascara `3` e nao `1`, porque a tag tem
2 bits e nao 1. Ver o codigo emitido em `vm/jit/x64/CodeGeneratorX64.c:1400-1404`
(`testq rax,3` / `testq rsi,3`). O prototipo de referencia usa 1 bit para
SmallInteger e 3 para o resto; aqui sao 2 e 2. **A janela do SmallFloat64 do
repositorio (62 bits de payload) e mais larga que a do prototipo, nao mais
estreita**, entao o item "considere imediatos de SmallFloat no estilo Spur" da
fase 6 do plano **ja esta feito** e nao precisa ser reimplementado, apenas
preservado.

**Bit 3 do ENDERECO** de um objeto de heap discrimina young (setado) de old
(limpo): `Object.h:25-34`, `isNewObject`/`isOldObject` em `Object.h:272-281`.
Isso obriga todo objeto de heap a ficar alinhado a 16 e obriga o mmap da young
space a deixar esse bit utilizavel. E uma invariante do modelo de memoria, nao
uma peculiaridade de x86. **Qualquer GC novo tem que reproduzir isso ou trocar
explicitamente o teste de geracao em todo lugar que o usa.**

## 2. Cabecalho de objeto

`vm/core/Object.h:16-23`:

```c
#define HEADER_SIZE (2 * sizeof(Value))     // 16 bytes
#define OBJECT_HEADER \
	struct RawClass *class; \               // 8 bytes: PONTEIRO, nao indice
	uint32_t hash; \
	uint8_t unused; \
	uint8_t payloadSize; \
	uint8_t varsSize; \
	uint8_t tags
```

**A classe e um PONTEIRO de 64 bits, nao um indice.** Nao existe tabela de
classes, nao existe indice de classe em lugar nenhum: `getClassOf`
(`vm/core/Class.h:59-73`) devolve `RawClass *`, e o guard do cache inline compara
o ponteiro tagueado inteiro contra o valor bakeado
(`CodeGeneratorX64.c:1204`, `cmpq [rax+IcState.class], rdi`).

Isto tem tres consequencias que o plano ja antecipa e uma que ele nao:

1. O guard de IC e um `cmp` de 64 bits contra um imediato de 64 bits, nao um
   `cmp` de 32. Custa bytes de codigo e um `movabs` a mais quando o imediato nao
   cabe em 32 bits.
2. Como o ponteiro de classe pode MOVER (classes sao objetos comuns e podem
   estar na young space), todo valor de classe bakeado no codigo precisa ser
   registrado em `pointersOffsets` para o coletor atualizar
   (`vm/core/CompiledCode.h:29`, `nativeCodePointersOffsets`).
3. **E por isso que todas as celulas de cache inline sao ZERADAS a cada
   scavenge** (secao 7). O endereco da classe morre com a epoca de GC, entao o
   perfil morre junto.
4. A que o plano nao cobre: `payloadSize`, `varsSize` e `tags` sao `uint8_t`, e o
   `unused` de 8 bits e o unico espaco realmente livre. Um indice de classe de 22
   bits **nao cabe em `unused`**; ele exige reorganizar o cabecalho (por exemplo
   `hash` de 32 bits e indice de classe de 22 bits compartilhando uma palavra, ou
   o formato Spur de 64 bits com classe de 22 bits + hash de 22 bits + formato +
   bits de GC). Isso e uma mudanca de modelo de objeto, ADR obrigatorio.

O formato da instancia vive na CLASSE, nao no objeto: `InstanceShape` em
`Object.h:59-67` (`payloadSize`, `varsSize`, `isIndexed`, `isBytes`,
`valueType`, `size`), lida por `computeRawObjectSize` (`Object.h:206-209`) toda
vez que o GC precisa do tamanho de um objeto. **Ou seja: o coletor faz duas
derreferencias (objeto -> classe -> shape) para saber o tamanho de cada objeto
que varre.** Um formato no proprio cabecalho eliminaria isso. Ha `InstanceShape`
pre-definidos para `Fixed`, `Indexed`, `String`, `Bytes`, `CompiledCode`,
`Block`, `Context`, `ExceptionHandler`, `UnwindHandler` e `Float`
(`Object.h:154-163`).

`RawClass` em `Object.h:69-81` tem 10 campos alem do cabecalho, incluindo
`namespace`. `RawMetaClass` em `Object.h:84-92`.

## 3. Conjunto de opcodes

**Baseado em REGISTRADORES, tres enderecos, 7 opcodes.** `vm/compiler/Bytecodes.h:10-18`:

```c
typedef enum {
	BYTECODE_COPY,              // destination:op, source:op
	BYTECODE_SEND,              // selector:lit, nargs:u8, receiver:op, args:op[]
	BYTECODE_SEND_WITH_STORE,   // ... + result:op
	BYTECODE_RETURN,            // source:op
	BYTECODE_OUTER_RETURN,      // source:op
	BYTECODE_JUMP,              // target:int32
	BYTECODE_JUMP_NOT_MEMBER_OF // class:lit, arg:op, target:int32
} Bytecode;
```

Nao ha pilha de avaliacao. Os "registradores" sao operandos tipados,
`Bytecodes.h:20-35`:

```
OPERAND_VALUE (Value de 64b imediato)   OPERAND_NIL/TRUE/FALSE/THIS_CONTEXT
OPERAND_TEMP_VAR (index)                OPERAND_ARG_VAR (index)
OPERAND_SUPER                           OPERAND_CONTEXT_VAR (index, level)
OPERAND_INST_VAR (index)                OPERAND_INST_VAR_OF (index, operando, level)
OPERAND_LITERAL (index)                 OPERAND_ASSOC (index)
OPERAND_BLOCK (index)
```

Indices de literal e de variavel sao de **16 bits** (`Bytecodes.h:42`), com
`level` de 8 bits para contextos aninhados.

Isso e uma descoberta que muda o custo do projeto para melhor: **o bytecode
atual ja tem exatamente a forma que o prototipo de referencia assume**
(`SEND` com destino, tres enderecos, registradores virtuais). O `bc.py` do
prototipo e este conjunto de opcodes com outros nomes. Nao ha impedance
mismatch entre "o que o parser produz" e "o que a construcao de SSA quer
consumir": `SEND_WITH_STORE` ja carrega o `ins.d` que o `lower.py` usa, e a
analise de vivacidade retroativa da fase 3 opera diretamente sobre indices de
temp/arg, que e o que `_uses_defs` faz no prototipo.

Codificacao: opcode de 1 byte, operandos com tag de 1 byte + payload. Emissao em
`Bytecodes.h:82-198`, leitura em `Bytecodes.h:201-293` (`BytecodesIterator`, que
expoe tanto `bytecodeOffset` em bytes quanto `bytecodeNumber` sequencial). Ha um
impressor completo em `Bytecodes.h:296-404`.

`vm/compiler/Compiler.c` (37k) faz o inlining ESTRUTURAL de controle de fluxo
(`ifTrue:`, `whileTrue:`, `to:do:`, `and:`, `or:`), emitindo
`JUMP_NOT_MEMBER_OF` contra `True`/`False` como guard. Aritmetica NAO e
resolvida no compilador de bytecode: sai como `SEND` normal. **A disciplina que
o plano exige ja e respeitada NO COMPILADOR.** Quem trapaceia e o codegen, ver
secao 6.

## 4. Layout de frame, prologo e epilogo

Ha prologo e epilogo canonicos, identicos em todo metodo framed.
`vm/jit/x64/CodeGeneratorX64.c:184-209`:

```
push  rbp
mov   rbp, rsp
sub   rsp, frameSize*8
mov   TMP, nil                  ; se ha algum slot a nilificar
mov   [rbp-8*(i+1)], TMP        ; para cada slot i, exceto os "mortos"
```

Epilogo, `CodeGeneratorX64.c:212-217`: `add rsp, frameSize*8` / `pop rbp` / `ret`.

Mapa do frame, de `vm/core/StackFrame.h:4-19` e `StackFrame.c:70-88`:

```
  [rbp + 8 + 8*i]   args[i]        (receiver em args[0]; empurrados pelo chamador)
  [rbp + 8]         parentIc       (endereco de retorno)
  [rbp + 0]         parent         (RBP salvo do chamador)
  [rbp - 8]         slot 0  = FRAME_CODE_OFFSET, entrada do NativeCode (R11)
  [rbp - 16]        slot 1  = CONTEXT_SLOT, o Context
  [rbp - 8*(i+1)]   slot i  = temp/spill, i >= 2
```

O acessor e `stackFrameGetSlot(frame, i) = ((Value *) frame - 1)[-i]`
(`StackFrame.c:77-81`), com um `PORT_ME(frame-layout)` explicito em
`StackFrame.c:66-69` dizendo que essa aritmetica ESPELHA o que
`generatePrologue` emite e que um backend novo precisa reproduzir exatamente ou
tornar os acessores parte do contrato por arquitetura.

`generateContextDefinition` (`CodeGeneratorX64.c:220-253`) roda logo depois do
prologo e sobrescreve os slots 0 e 1. `jit/PrologueSlots.h` documenta quais nils
do prologo sao mortos por isso e por que pular exatamente um deles NAO e seguro
(metodo framed com `hasContext`: a alocacao do contexto e um ponto de GC e o
slot ainda esta por escrever). O prologo custa 12,5% das instrucoes retiradas do
Richards, medido.

Metodos podem ser **frameless** (`regsAlloc.frameLess`, `CodeGeneratorX64.c:149`):
sem prologo, so `ret`. Blocos frameless nao tem frame nenhum.

**Alocacao de registradores hoje**: `vm/jit/RegisterAllocator.c` (640 linhas), linear
scan SEM splitting, com `Variable` carregando `start`, `end`, `gcEnd`, `reg` e
`frameOffset` (`RegisterAllocator.h:27-41`). Ha um unico banco de registradores,
inteiro; **XMM nao e alocado**, e usado ad hoc dentro dos caminhos rapidos de
float e nunca sobrevive a um send. `SPILLED_REG = -1`.

## 5. Como o GC acha as raizes em frames nativos

**Hibrido: stackmaps precisos escolhem os slots, varredura defensiva valida o
conteudo.** Esta e a descoberta mais importante da fase 0 para a fase 3.

O caminho: `vm/memory/Scavenger.c:282-322` (`iterateStackFrames`) anda a cadeia
de frames de cada fiber. Para cada frame:

1. **Os argumentos sao varridos INCONDICIONALMENTE**, sem consultar stackmap:
   `Scavenger.c:295-301`, `argsSize = code->argsSize + 1` (o `+1` e o receiver).
   O unico filtro e `valueTypeOf(*value, VALUE_POINTER)`.
2. **Os slots locais vem do stackmap**, achado pelo IC de retorno:
   `findStackmap(code, prev->parentIc)` (`Scavenger.c:303`,
   `jit/CodeDescriptors.h:194-206`). O stackmap e um bitset (`RawStackmap`,
   `CodeDescriptors.h:8-14`) alocado como `ByteArray` e guardado no
   `NativeCode.stackmaps`, que e um `RawArray` VARRIDO pelo GC.
3. Cada slot marcado passa por `scavengeStackSlot` (`Scavenger.c:239-279`), que
   **NAO confia no stackmap**: se o valor aponta para fora do espaco sendo
   evacuado, ou falha em `plausibleObject` (`Scavenger.c:197-225`: alinhamento a
   16, classe mapeada, metaclasse mapeada, tamanho menor que um semispace), o
   slot e SILENCIOSAMENTE reescrito para `nil`.

Isso e reparo defensivo por design, documentado em `Scavenger.c:169-182`: o
alocador linear-scan e cego a fluxo de controle e marca como vivo o slot de
resultado de um `ifTrue:ifFalse:` em call sites do outro braco, onde ele ainda
contem o nil do prologo ou lixo. **A consequencia para nos e severa: um bug de
"esqueci de descrever um slot" nao produz falha de teste, produz reparo
silencioso.** Ja registrado como licao no historico do projeto; a guarda certa e
`ASSERT` no codegen, nao teste.

**Os stackmaps NAO distinguem ponteiro de valor cru.** `generateStackmap`
(`CodeGeneratorX64.c:2637-2670`) marca um slot quando a variavel esta viva por
`[start, gcEnd]` e esta `VAR_ON_STACK`. Todo slot marcado e tratado como tagged.

**Mas o buraco para valores crus ja esta furado e nunca foi usado:**
`CodeGenerator.frameRawAreaSize` (`vm/jit/CodeGenerator.h:19`) participa do
calculo do tamanho e do deslocamento de indice do stackmap
(`CodeGeneratorX64.c:2642`, `2657`, `2665`, e identicamente no ppc64 em
`CodeGeneratorPpc64.c:2366`, `2378`, `2386`), e e escrito em UM unico lugar, com
o valor zero (`vm/jit/StubCode.c:36`). Ou seja: existe uma area de frame reservada
para valores nao varridos, com o indice do stackmap ja deslocado para acomoda-la,
e ela tem tamanho zero em todo metodo que o VM ja compilou. **Para a fase 3 e a
fase 6 isso e exatamente o gancho que faltava, e ele foi projetado de proposito.**

**Safepoints**: `generateSafepointPoll` (`CodeGeneratorX64.c:286-319`), emitido
(a) na entrada de todo metodo framed (`CodeGeneratorX64.c:161`) e (b) em **toda
back-edge de laco** (`CodeGeneratorX64.c:578`). O poll le
`heap->safepointRequested` da TLS e, se setado, chama `heapGcPoll` com stackmap.
Em back-edge o stackmap e SUPERAPROXIMADO (`overapproxStackmap`,
`CodeGenerator.h:30`), marcando todo temp spillado, porque a liveness
linear-scan omite ponteiros carregados pelo laco.

Todo `generateCCall` com `storeIp=1` gera um stackmap
(`CodeGeneratorX64.c:2677-2681`), e todo send gera um stackmap depois do `call`
(`CodeGeneratorX64.c:1561`). **Ou seja: ja existe um stackmap em cada ponto onde
uma deoptimizacao poderia precisar reconstruir o estado.** O que nao existe e a
descricao de VALOR (que registrador/slot corresponde a qual variavel de bytecode
em qual offset), so a descricao de RAIZ.

Ha tambem `GarbageCollector.c` (mark and sweep da old space, 616 linhas) e
`RememberedSet.h` para o barrier de geracao; o barrier emitido pelo JIT e
`generateStoreCheck` (`CodeGeneratorX64.c:1580`).

## 6. Cache inline: existe, e onde o send e resolvido

**Existe, e e bom.** `vm/jit/InlineCache.h` + `InlineCache.c` (358 linhas).

- Uma `IcCell` por sitio de send dinamico, guardada DENTRO do `NativeCode`,
  depois do codigo e dos offsets de patch (`CompiledCode.h:44-58`).
- `IcState` imutavel apos publicada, trocada so por CAS de uma palavra alinhada
  (`InlineCache.h:49-63`). Rede lattice: `unlinked -> mono -> pic(2..6) -> mega`,
  com `IC_PIC_CAPACITY = 6` (`InlineCache.h:35`). Mega e permanente.
- Guard inline emitido em `generateIcGuard` (`CodeGeneratorX64.c:1196-1207`): 4
  instrucoes, carrega o estado da celula, compara `state->class` com o RDI
  (classe do receiver, tagueada) e pega `state->target`.
- Miss vai para `PicProbeStub` (`vm/jit/x64/StubCodeX64.c`), que caminha as vias
  do PIC ou chama `inlineCacheMiss` em C.

**O que a IC de hoje NAO tem, e a fase 2 precisa:**

| item | hoje |
|---|---|
| contagem por classe | NAO, so presenca (`IcWay` = `{class, target}`, `InlineCache.h:44-47`) |
| classe do primeiro argumento | NAO, so do receptor |
| flag explicita de megamorfico | SIM (`IC_KIND_MEGA`) |
| sobrevivencia a GC | **NAO** |

Esse ultimo e o problema estrutural. `icResetNativeCodeCells` roda a **cada
scavenge** (`Scavenger.c:575-583`) e a cada mark-sweep
(`GarbageCollector.c:338-345`), devolvendo toda celula mono/pic ao estado
unlinked, porque a celula guarda um ENDERECO de classe e o endereco morre com a
epoca. `Tier.h:121-129` reconhece o problema em texto: distingue "sitio que nunca
rodou" de "sitio cujo perfil o coletor jogou fora" e chama a persistencia de
feedback de "conserto barato". **Com um indice de classe estavel no cabecalho
(secao 2), o reset deixa de ser necessario e o perfil passa a ser cumulativo.
Esses dois itens sao a mesma decisao.**

**Onde o send e resolvido, em ordem** (`generateSend`, `CodeGeneratorX64.c:1332-1577`):

1. `classifyIdentity` (`jit/SendClassify.h:65-75`): `==`, `~~`, `isNil`,
   `notNil` compilam para comparacao inline, **sem guard, sem dispatch, sem
   celula de IC**.
2. `classifyArith` (`SendClassify.h:27-53`): `+ - * / < > <= >= = ~= bitAnd:
   bitOr: bitXor:` ganham caminho rapido inline de SmallInteger
   (`CodeGeneratorX64.c:1396-1447`) e, em seguida, caminho rapido de Float em SSE
   (`generateFloatFastPath`, `CodeGeneratorX64.c:911`).
3. Se nada disso resolveu: `compiledCodeResolveOperandClass`
   (`vm/core/CompiledCode.h:387-409`) tenta descobrir a classe do receptor
   estaticamente (literal, `nil`, `true`, `false`, `thisContext`, `super`,
   bloco). Se descobre E `lookupSelector` acha o metodo, o send e
   **devirtualizado em tempo de compilacao**, com a entrada bakeada e SEM guard
   e SEM celula de IC (`CodeGeneratorX64.c:1489-1542`). Um thunk inline de
   re-resolucao cobre redefinicao de classe (`jit/SpecSite.h`, `SPEC_STATIC`).
4. So o que sobra vira sitio de IC (`generateIcSendPromoted`,
   `CodeGeneratorX64.c:1262`).

**Achado que contradiz a premissa da fase 2 e que eu preciso destacar:** quando o
caminho rapido de aritmetica ACERTA, ele pula para `arithMerge`, que e bindado
DEPOIS do `call` (`CodeGeneratorX64.c:1566-1574`). A celula de IC desse sitio
nunca e tocada. Portanto **hoje, num laco de `total := total + x` com dois
SmallIntegers, o perfil daquele sitio fica permanentemente vazio**, e quando nao
fica, ele descreve apenas os casos que FALHARAM o caminho rapido, ou seja
exatamente o complemento do comportamento tipico. Nao e que o perfil de
aritmetica seja impreciso: ele e sistematicamente enviesado ao contrario. Isso
confirma, com o codigo na mao, a disciplina que o plano exige, e da o motivo
concreto: nao e purismo, e que o feedback existente esta invertido.

## 7. Coleta de perfil

Existe uma, indireta, e uma camada de tiering em cima dela.

**Perfil**: as proprias celulas de IC (secao 6). Nada mais. Nao ha contador de
execucao por bytecode, nem por aresta, nem histograma de tipo, nem contador de
laco. Nao ha amostragem.

**Tiering**: `vm/jit/Tier.h` + `Tier.c` (413 linhas). Todo metodo framed com pelo menos
um send dinamico nasce em tier 0 com um contador regressivo alocado por `malloc`
(`tierAllocCounter`, `Tier.h:163`, deliberadamente fora do `NativeCode` porque um
store em `insts-8` dispara o detector de codigo automodificavel do x86, medido em
4x no Richards). O prologo decrementa e, ao chegar a zero, chama `tierRecompile`
(`CodeGeneratorX64.c:336-365`). Limiar default 1000, `ST_TIER_THRESHOLD`.

A recompilacao tier 1 roda o MESMO codegen sobre os MESMOS bytecodes com
`tierFeedback` apontando para o `NativeCode` superado, e faz duas coisas:

1. promove sends monomorficos a guard de classe exata + chamada direta bakeada
   (`generateIcSendPromoted`);
2. inlina o corpo do callee no nivel de BYTECODE
   (`vm/compiler/Optimizer.c`, 940 linhas), com teto de 192 bytes
   (`Tier.h:247-258`), remapeando temps e emitindo o guard como
   `JUMP_NOT_MEMBER_OF`.

**E aqui esta o limite estrutural que o projeto novo existe para remover, dito
no proprio codigo** (`Tier.h:13-16`):

> There is NO deoptimization and NO on-stack replacement: every speculation
> keeps today's send as its floor.

Ou seja: toda especulacao precisa carregar seu proprio fallback. Nao da para
apagar uma alocacao, nao da para promover um phi, nao da para eliminar um guard
que estabeleceu um fato, porque nao existe caminho de volta. O teto do sistema
atual e "melhorar o dispatch", nunca "mudar a representacao".

Existe tambem `SpecSite` (`jit/SpecSite.h`) para envenenar sitios especulativos
quando uma classe e redefinida, e contadores `TypeStats` (`Tier.h:88-117`) de um
censo de sends ja realizado. Ha um veredito registrado desse censo em
`docs/type-annotation-measurement.md`: anotacao de tipo removeria dispatch em
cerca de 1% dos sends quentes, porque 78% dos sitios anotaveis que executam ja
sao monomorficos. Isso NAO invalida nada do plano atual (o plano nao aposta em
anotacao para dispatch; aposta nela para LAYOUT, que e outra coisa), mas explica
por que a classe de valor da fase 7 e um item de representacao e nao de tipos.

## 8. O que `benchmarks/` mede hoje, e como se roda

Seis programas, todos self-checking (levantam erro se o resultado estiver
errado), todos imprimindo o TOTAL do lote em milissegundos:

| arquivo | o que exercita |
|---|---|
| `Richards.st` | escalonador de tarefas: dispatch polimorfico, condicionais aninhados, blocos como corpos de tarefa, listas ligadas |
| `DeltaBlue.st` | solver de restricoes: alocacao e churn de GC, hierarquias, `OrderedCollection` |
| `FloatBench.st` | aritmetica de ponto flutuante |
| `MixedArithBench.st` | aritmetica mista int/float, com checagem bit-exata contra referencia em C |
| `ArrayNumericBench.st` | percurso numerico de `Array` |
| `BigIntBench.st` | LargeInteger |

`benchmarks/` e **GPL v2** (Richards e DeltaBlue sao de Maloney e Wolczko),
isolado do resto do repositorio que e BSD. Nao entra em `samples/`, nao e linkado
em nada.

Como roda:

```sh
./run_benchmarks.sh                 # build + bootstrap + roda todos
./run_benchmarks.sh --no-build
BUILD=mybuild ./run_benchmarks.sh
```

Ferramenta de comparacao A/B, que e o que importa para as fases seguintes:

```sh
scripts/ab.sh 'base=' 'novo=ST_ALGO=1' 8 > /tmp/run.jsonl
awk -f scripts/report.awk /tmp/run.jsonl
```

Tres propriedades dela que o plano deve herdar, nao reinventar
(`benchmarks/README.md:110-141`):

- **intercala ABBA**, porque layout de codigo neste VM e loteria: mover um bloco
  frio ja produziu 2% reproduziveis com instrucoes identicas, e mudar alinhamento
  de cabecalho de laco de 16 para 32 bytes moveu um benchmark em 4%;
- **registra instrucoes retiradas via `perf`**, nao so relogio: nessa carga a
  contagem de instrucoes repete a poucas partes em 10^8;
- **toma o MINIMO de N execucoes, nao a media**, porque o ruido e unilateral;
- **recusa dar veredito** que nao consegue ver, por teste de sinal pareado.

`benchmarks/results/BASELINE.jsonl` e o registro versionado; a instrucao e
APENDAR, nunca reescrever.

**WARNING historico que importa para a fase 1**: por toda a historia do projeto os
benchmarks imprimiam a media por iteracao com divisao INTEIRA. Richards imprimia
`total // 100` e reportava 6, 7 ou 8 ms e nada mais, para sempre. O harness era
CEGO e respondia INCONCLUSIVO em todas as medicoes. **A licao e que a
instrumentacao da fase 1 tem que ser validada contra um efeito conhecido antes de
ser usada para decidir qualquer coisa.**

---

## 9. Inventario para a reescrita

O pedido mudou durante a fase 0: em vez de coexistir com o JIT atual atras de
`--jit=v1|v2`, remover codegen, JIT e GC e refazer, mantendo parser, geracao de
pacotes e a maior parte do runtime. Este e o inventario correspondente.

### Sai inteiro

- `vm/jit/` (3493 + 6592 x64 + 6181 ppc64): assembler, codegen, alocador de
  registradores, IC, tier, stubs, descritores, perf map.
- `vm/memory/` (3544): heap, scavenger, mark-sweep, freelist, remembered set,
  safepoint, paginas.
- `vm/compiler/Optimizer.c` (940 linhas): o inliner de bytecode do tier 1. Ele existe
  para compensar a falta de uma IR; com SSA ele nao tem razao de ser.

### Fica, sem tocar

- `vm/compiler/`: `Tokenizer`, `Parser`, `Ast.h`, `Scope`, `Compiler.c`,
  `Bytecodes.h`. O bytecode ja tem a forma certa (secao 3).
- `vm/tools/`: `Bootstrap.c`, `Project.h`, `Cli.h`, `Repl.c`.
- `packages/`, `samples/`, `tests/`, `run_tests.sh`, `scripts/ab.sh`.

### Fica, mas tem que acompanhar

- `vm/core/Object.h`: o tagging fica (esta certo e o SmallFloat64 ja e o que a
  fase 6 pede). O CABECALHO muda, se formos ao indice de classe. ADR.
- `vm/core/Class.h`, `Entry.c`, `Handle.h`, `Lookup.c`, `Thread.h`,
  `StackFrame.h/.c`, `CompiledCode.h`: todos falam diretamente com o codegen e/ou
  com o GC. `StackFrame` E o contrato de frame.
- `vm/tools/Snapshot.c`: serializa o modelo de objeto. Muda com ele. Atenuante
  importante: `run_tests.sh` e `run_benchmarks.sh` fazem bootstrap de imagem NOVA
  a partir de `packages/Core` a cada execucao, entao o snapshot e derivado, nao
  precioso. **Isso e o que torna a troca de modelo de objeto viavel.**
- `vm/runtime/Primitives.c` (3279 linhas, 175 entradas: 41 `GEN` + 134 `CCALL`,
  `Primitives.c:269`): as 134 `CCALL` sao funcoes C comuns e sobrevivem quase
  intactas; as 41 `GEN` **sao geradores de codigo de maquina** e morrem com o
  codegen. A ordem da tabela e o numero de primitiva gravado nos
  `CompiledMethod` do snapshot, mas como o snapshot e regerado, isso deixa de ser
  uma amarra.
- `vm/concurrency/`: fibers e scheduler tocam frames nativos e safepoints.

### Consequencia que eu preciso colocar na mesa

Com JIT e GC fora ao mesmo tempo, **nada executa ate que os dois estejam de
volta**, e `run_tests.sh` nao pode ficar verde a cada commit como o plano
original exigia. As duas regras sao incompativeis como escritas, e essa e a
unica decisao desta fase que eu nao vou tomar sozinho. Ver a pergunta no fim.

---

## 10. Rota A ou rota B: recomendacao

**Recomendo a ROTA B**, tier 1 template JIT com mapa bytecode-para-codigo e
frame estavel, sem interpretador. Com uma ressalva sobre o que muda agora que a
reescrita e do zero.

### O argumento decisivo nao e o custo do interpretador

Escrever um interpretador para 7 opcodes de tres enderecos e trabalho pequeno,
talvez 1200 linhas. Se fosse so isso, a rota A ganharia pelo alvo de deopt
trivial. **Nao e so isso.**

**1. As primitivas nao sao funcoes C.** Das 175 entradas da tabela
(`Primitives.c:269`), **41 sao GERADORES DE CODIGO DE MAQUINA** (`GEN`,
enumeradas em `jit/PrimitivesGen.def`), nao implementacoes em C. E nao sao as
faceis: `BlockValuePrimitive0..3`, `BlockValueArgsPrimitive`,
`BlockWhileTruePrimitive`, `BlockWhileTrue2Primitive`,
`BlockOnExceptionPrimitive`, `BlockUnwindPrimitive`, `ExceptionSignal`,
`MethodSendPrimitive`. Essas manipulam o frame nativo, a cadeia de unwind por
fiber e os contextos diretamente. Um interpretador precisa de uma segunda
implementacao de cada uma, e "segunda implementacao de `BlockUnwindPrimitive`"
significa uma segunda semantica de unwind convivendo com a primeira na mesma
pilha de fiber.

**2. O custo real da rota A e duplicar o numero de motores de execucao que todo
subsistema transversal precisa entender**, e a lista desses subsistemas neste VM
e longa e cada um deles ja e delicado:

- raizes de GC: `iterateStackFrames` (`Scavenger.c:282`) anda frames nativos por
  stackmap; frames de interpretador precisariam de outro esquema, intercalado na
  MESMA pilha de fiber;
- unwind de excecao: cadeia de `UnwindHandler` por fiber, com cleanups nos tres
  caminhos, ja implementada sobre frames nativos;
- reflexao de contexto: `thisContext`, `Context>>parent/receiver/argumentAt:/
  temporaryAt:`, e `contextFrameOnCurrentStack` (`StackFrame.c:121-140`), que
  valida um frame checando se ele esta dentro do span da pilha do fiber ATUAL;
- backtrace: `printBacktrace` (`StackFrame.c:143-179`);
- safepoints e stop-the-world multicore;
- migracao/pinning de fiber.

**3. Do lado da rota B, o que ela exige a mais JA EXISTE neste repositorio**, e
isso e o que inverte a comparacao:

- **mapa bytecode -> codigo de maquina**: `createBytecodeDescriptor(nativePos,
  bytecodePos)` (`CodeDescriptors.h:67-75`), com busca por posicao nativa
  (`descriptorsAtNativePosition`, `CodeDescriptors.h:141`), ja emitido em todo
  send (`CodeGeneratorX64.c:1562`), ja guardado em `NativeCode.descriptors`, ja
  com encoding que mantem os 16 bits baixos zerados para o `Value` ler como
  SmallInteger e o GC varrer sem se incomodar. Mais: `generateBody` **ja calcula
  `machineOffsetAt[]` para cada offset de bytecode** (`CodeGeneratorX64.c:384`,
  usado em 571 para ligar back-edges) **e joga fora no fim da funcao**. O mapa
  que a rota B precisa e literalmente computado hoje e descartado.
- **frame estavel no tier 1**: o prologo ja e canonico e identico em todo metodo
  (secao 4), com os acessores em `StackFrame.c` como contrato declarado e o
  backend ppc64 reproduzindo o mesmo layout.
- **stackmap em todo ponto de retomada plausivel**: todo send e todo `CCall` com
  `storeIp` ja emitem um (secao 5).
- **area de frame para valores crus**: `frameRawAreaSize` ja esta no calculo do
  stackmap nos dois backends, valendo zero (secao 5).

A rota B tambem e o que o proprio prototipo modela: o estado de deopt e
`(metodo, bci, mapa slot->valor)` e o alvo e "o codigo de tier 0 naquele bci". Se
tier 0 e um interpretador ou um template JIT com mapa bci->pc e detalhe de
implementacao DO ALVO, e este repositorio ja tem o mapa.

### O que custa na rota B, sem maquiagem

**O layout de frame do tier 1 tem que virar invariante.** Hoje o
`Variable.frameOffset` sai de um linear scan cuja atribuicao depende da
liveness, entao a mesma variavel de bytecode pode cair em slots diferentes em
compilacoes diferentes. Um escritor de deopt precisa de um mapeamento
conhecido. A correcao e o tier 1 ter layout FIXO: slot i = variavel de bytecode
i, sem promocao a registrador alem de um conjunto fixo. Isso e exatamente o que
"template JIT" significa e e MAIS SIMPLES que o que existe, mas deixa o tier 1
mais lento que o tier 0 de hoje, e esse custo tem que ser medido e registrado,
nao estimado.

Segundo custo: a rota A daria de graca um alvo de deopt neutro de arquitetura, o
que ajudaria os ports ppc64. A rota B nao. Isso e real e nao muda o veredito,
porque o alvo declarado e x86-64 e porque os dois backends POWER estao hoje fora
do gate default de `run_tests.sh` (nada em `run_tests.sh` ou `build.sh` os
invoca; eles rodam por `scripts/ppc64/` sob QEMU, manualmente).

### O que muda com a reescrita do zero

Se o codegen inteiro vai ser refeito, a rota B fica ainda mais favoravel, por um
motivo que nao existia quando o plano foi escrito: **nao ha divida a pagar**. A
objecao normal a rota B e "exige um mapa bytecode->maquina e um frame estavel que
o JIT existente nao tem, e retrofitar isso e caro". Retrofit deixou de ser a
questao. Projetar o tier 1 desde o inicio com layout fixo e mapa por bytecode
custa menos que projeta-lo sem, porque o mapa tambem e o que da OSR, backtrace
exato e o `bci` que o `Context` reflexivo ja precisa hoje.

**Rota A permanece disponivel como plano B barato**, e vale registrar: se em
algum momento o `--deopt-stress` da fase 3 se mostrar impossivel de fechar contra
o tier 1, um interpretador restrito (sem primitivas GEN, usado SO como oraculo de
verificacao diferencial, nunca em producao) e uma ferramenta legitima. Isso e
diferente de adotar a rota A, e nao carrega o custo dos seis subsistemas.

---

## 11. Achados que restringem o plano

Lista curta do que descobri que o plano precisa absorver.

1. **A tag de SmallInteger tem 2 bits, nao 1.** O guard `or` + `test` e contra
   `3`. O prototipo assume 1 bit. Nao muda a tecnica, muda a constante.
2. **SmallFloat64 imediato ja existe e e mais largo que o do prototipo.** O item
   correspondente da fase 6 esta feito. Preservar, nao reimplementar.
3. **Nao ha indice de classe e `unused` tem 8 bits.** O `cmp` de 32 bits da fase
   6 exige remodelar o cabecalho do objeto, nao preencher um campo livre. ADR.
4. **O perfil e destruido a cada scavenge** porque a celula guarda endereco de
   classe. Indice de classe e persistencia de perfil sao a mesma decisao.
5. **O caminho rapido de aritmetica pula o IC quando acerta**, entao o perfil
   dos sitios aritmeticos hoje e o complemento do comportamento tipico. A
   disciplina da fase 2 nao e purismo, e correcao de um vies medido.
6. **`compiledCodeResolveOperandClass` devirtualiza sends estaticamente** sem
   guard e sem celula. Sob a fase 2 isso sai.
7. **O GC repara slots implausiveis para nil silenciosamente**
   (`Scavenger.c:239-279`). Bug de stackmap nao vira teste vermelho. A guarda tem
   que ser `ASSERT` no codegen.
8. **`frameRawAreaSize` existe, esta cabeado nos dois backends e vale zero.** E o
   gancho da fase 3 e da fase 6, ja projetado.
9. **`machineOffsetAt[]` ja e computado por bytecode e descartado.** E o mapa da
   rota B.
10. **41 das 175 primitivas sao geradores de codigo de maquina**, incluindo toda
    a familia de bloco/excecao/unwind. Isso e o que torna caro qualquer motor de
    execucao alternativo.
11. **O snapshot e derivado**: `run_tests.sh` regera a imagem a partir de
    `packages/Core` a cada execucao. Modelo de objeto pode mudar sem migracao.
12. **Layout de codigo neste VM vale ate 4%** com instrucoes identicas
    (`benchmarks/README.md:112-118`). Nenhum ganho abaixo disso e reivindicavel
    sem A/B intercalado.
