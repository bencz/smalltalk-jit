# O contrato do corte

O que `vm/jit/` e `vm/memory/` forneciam ao resto do VM, extraido pelo linker e
nao por leitura: simbolos definidos dentro desses dois diretorios e referenciados
de fora deles, no commit `583047d`.

```sh
# como foi obtido, para poder refazer
cd build/CMakeFiles/VM.dir/vm
for o in $(find jit memory -name '*.o'); do nm --defined-only "$o" | awk '{print $3}'; done | sort -u > defined
for o in $(find . -name '*.o' | grep -v '^./jit' | grep -v '^./memory'); do nm -u "$o" | awk '{print $2}'; done | sort -u > needed
comm -12 defined needed
```

**77 simbolos.** Esta e a superficie que a implementacao nova tem que oferecer
para o VM voltar a linkar, e nada alem dela e obrigacao.

## 1. Primitivas geradas: 41 simbolos

`generateAtPrimitive`, `generateAtPutPrimitive`, `generateSizePrimitive`,
`generateInstVarAtPrimitive`, `generateInstVarAtPutPrimitive`,
`generateIdentityPrimitive`, `generateHashPrimitive`, `generateClassPrimitive`,
`generateBehaviorNewPrimitive`, `generateBehaviorNewSizePrimitive`,
`generateCharacterNewPrimitive`, `generateCharacterCodePrimitive`,
`generateBlockValuePrimitive0Args`..`3Args`, `generateBlockValueArgsPrimitive`,
`generateBlockWhileTrue`, `generateBlockWhileTrue2`,
`generateBlockOnExceptionPrimitive`, `generateBlockUnwindPrimitive`,
`generateExceptionSignalPrimitive`, `generateStringHashPrimitive`,
`generateIntAddPrimitive`, `generateIntSubPrimitive`, `generateIntMulPrimitive`,
`generateIntQuoPrimitive`, `generateIntModPrimitive`, `generateIntRemPrimitive`,
`generateIntFloorDivPrimitive`, `generateIntNegPrimitive`,
`generateIntLessThanPrimitive`, `generateIntAndPrimitive`,
`generateIntOrPrimitive`, `generateIntXorPrimitive`, `generateIntShiftPrimitive`,
`generateInterruptPrimitive`, `generateExitPrimitive`,
`generateMethodSendPrimitive`, `generateMethodSendArgsPrimitive`,
`generateCCallPrimitive`, `generateNotImplementedPrimitive`.

Sao **geradores de codigo de maquina**, nao funcoes C, e essa e a razao pela
qual a rota A foi descartada (ADR 0001). Elas morrem com o codegen. A tabela
`Primitives[]` (`vm/runtime/Primitives.c:269`) continua sendo a fonte da verdade
dos NUMEROS de primitiva, que nao podem ser reordenados, mas as entradas `GEN`
precisam de uma estrategia nova. Tres opcoes, a decidir com ADR quando a fase
chegar:

1. reimplementar como geradores, como hoje;
2. reimplementar como funcoes C `CCALL` e deixar o inlining para o otimizador,
   que passa a ver `at:`/`size` como IR e nao como codigo opaco;
3. hibrido: `CCALL` como semantica de referencia e nos da IR para as que o
   otimizador precisa enxergar (`at:`, `at:put:`, `size`, aritmetica).

A opcao 3 e a que combina com a disciplina do ADR 0004, mas nao esta decidida.

## 2. Heap e GC: 20 simbolos

Alocacao: `allocate`, `allocateObject`.
Ciclo de vida: `initHeap`, `freeHeap`.
Coleta: `collectGarbage`, `LastGCStats`, `printHeap`.
Mutators: `heapAddMutator`, `heapEndMutator`, `heapFillAllTlabTails`.
Stop-the-world: `heapGcBegin`, `heapGcEnd`, `heapGcEnterBlocked`,
`heapGcLeaveBlocked`.
Locks: `heapCodegenLockEnter`, `heapCodegenLockLeave`, `heapSymbolLockEnter`,
`heapSymbolLockLeave`, `heapMonitorEnterStripe`, `heapMonitorExitStripe`.
Iteracao de paginas: `pageSpaceIteratorInit`, `pageSpaceIteratorNext`.

Os locks e o handshake de safepoint sao requisito R8 do ADR 0003: o VM e
multi-worker e isso nao esta em questao. `pageSpaceIterator*` existe porque os
coletores caminham a exec space para resetar celulas de IC; se o desenho novo
tornar isso desnecessario (indice de classe, requisito R4), o par sai.

## 3. Nucleo do JIT: 16 simbolos

`generateMethodCode` (compilacao sob demanda, chamada por `getNativeCode`),
`generateDoesNotUnderstand`, `getStubNativeCode`,
`SmalltalkEntry` e `targetCallSmalltalkEntry` (entrada C para Smalltalk),
`fiberSwitchAsm` e `fiberTargetPrimeStack` (troca de fiber, em assembly),
`gIcStats`, `gTierStats`, `gTypeStats`, `gTierSpecGuard`, `typeStatsNoteSend`
(contadores e censo, que somem: a instrumentacao nova e `core/Instrument.h`),
`icInvalidateAllSends` (invalidacao de send caches na cirurgia reflexiva de
method dictionary, que PRECISA continuar existindo).

## 4. Tipos, alem dos simbolos

Estao embutidos em estruturas que ficam, entao o desenho novo tem que decidir
sobre eles cedo:

- `NativeCode` (`vm/core/CompiledCode.h:16`), com `IcCell[]` e `SpecSite[]`
  inline depois do codigo. `RawCompiledMethod` guarda um `NativeCode *`.
- `RawStackmap` e os descritores (`jit/CodeDescriptors.h`), referenciados por
  `NativeCode` e varridos pelo GC.
- `Heap`, `Scavenger`, `PageSpace`, `TLAB`, `RememberedSet` (`memory/*.h`),
  com `Thread` (`vm/core/Thread.h`) guardando `tlab` e `rememberedSet` inline.
- `AssemblerBuffer` (`jit/Assembler.h`), que `compiler/Bytecodes.h` usa como
  buffer de emissao de BYTECODE. Isso e um acoplamento acidental: o emissor de
  bytecode nao deveria depender de um header do JIT. O desenho novo separa os
  dois.

## Linha de base do v1, para nao se perder

Medida no commit `583047d` antes do corte, na maquina de desenvolvimento:

| | |
|---|---|
| `run_tests.sh` | 158 ok, 3375 ms |
| `benchmarks/Vec3Boxed.st`, lote de 500 | 216 ms |
| alocacoes na regiao quente (200 elementos, 50 iteracoes) | **10.006** |
| sends na mesma regiao | **160.153** |
| bytes alocados | 488.480 |
| resultado numerico | **149876875.0** |

O resultado numerico e o mesmo que o prototipo de referencia em Python produz
(`python_proto/validate.py`), o que faz dele uma verificacao cruzada entre duas
implementacoes independentes e nao um teste de autoconsistencia. **E o valor que
o criterio de aceitacao chama de "resultado numerico identico ao do JIT atual".**
