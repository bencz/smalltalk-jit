# ADR 0009: macro assembler neutro, e nenhum registrador nomeado fora do backend

Data: 2026-07-27. Status: aceito. Corrige um erro ja cometido.

## O erro

A primeira versao de `vm/jit/Jit.c` emitia chamando `asmMovRegMem(buffer, RAX,
...)` diretamente. Duas coisas erradas de uma vez:

1. **registradores hard-coded.** `RAX` e `RCX` aparecem espalhados pelo
   compilador como se fossem parte do algoritmo. Nao sao: sao uma escolha do
   backend x86-64;
2. **um so alvo.** O compilador inteiro estava escrito contra x86-64, quando o
   projeto ja teve backends ppc64 big-endian e little-endian funcionando, e vai
   ter ARM e ABIs diferentes (SysV, Win64, AIX).

Isto e uma REGRESSAO. O VM anterior tinha a separacao certa
(`vm/jit/Target*.h` como contrato, `vm/jit/<arch>/` como implementacao,
`vm/jit/<arch>/abi/<abi>/` para a ABI) e ela foi perdida por eu escrever o
caminho mais curto primeiro.

## Decisao

Tres camadas, e o compilador de template so enxerga a primeira.

**1. Macro assembler neutro** (`vm/jit/MacroAssembler.h`). Um vocabulario de
operacoes com significado no nivel do bytecode, nao no nivel da instrucao de
maquina:

```
maPrologue / maEpilogue          maLoadSlot / maStoreSlot
maLoadImmediate                  maLoadField / maStoreField
maBranch / maBranchIfEqual       maTestTagAndBranch
maCompareClassAndBranch          maCallRuntime
```

O compilador de template chama SO isso e **nunca nomeia um registrador**. Cada
backend implementa cerca de vinte operacoes.

**2. Papeis de registrador, nao registradores.** O macro assembler possui seus
proprios scratch e os expoe, quando precisa, como PAPEIS (`MA_SCRATCH0`,
`MA_SCRATCH1`, `MA_RESULT`). Quem decide que `MA_SCRATCH0` e `RAX` no x64 e
`r11` no ppc64 e o backend, e ninguem mais precisa saber.

**3. Descricao de ABI** (`vm/jit/Abi.h`), um struct de dados e nao de codigo:
registradores de argumento em ordem, registrador de retorno, conjunto
callee-saved, alinhamento de pilha no ponto de chamada, shadow space. Uma
instancia por par (arquitetura, ABI): x64+sysv, x64+win64, ppc64+elfv1,
ppc64le+elfv2, arm64+aapcs.

## Por que o seam fica no macro assembler e nao no assembler cru

O VM antigo punha o seam no assembler cru, e o preco foi **duplicar o gerador de
codigo inteiro por arquitetura**: 2875 linhas de `CodeGeneratorX64.c` e um
`CodeGeneratorPpc64.c` de tamanho comparavel, com a mesma logica de bytecode
escrita duas vezes e divergindo. Boa parte dos bugs de port catalogados no
projeto sao exatamente divergencias entre as duas copias.

Com o seam no macro assembler, a logica de bytecode existe UMA vez e o que se
porta sao as vinte operacoes. Um backend novo e um arquivo, nao um gerador.

O preco e que uma sequencia especializada de uma arquitetura (um endereçamento
indexado do POWER, um `ldp` do ARM) nao aparece automaticamente. Aceito: o
tier 1 e um compilador de template, onde a virtude e ser previsivel, e o tier 2
tera seu proprio backend com alocacao de registradores de verdade.

## Alocacao de registradores

**Nao existe no tier 1, por desenho.** Um template JIT com frame fixo (slot i =
registrador i do bytecode) e o que torna o mapa de desotimizacao escrivel
(ADR 0001), e alocar registradores destruiria exatamente essa propriedade. Os
scratch do macro assembler sao suficientes porque cada bytecode compila para uma
sequencia fechada que comeca e termina na memoria.

A alocacao de verdade, linear scan com splitting e bancos separados para
inteiros e ponto flutuante, e do backend de SSA (fase 6), e la ela consome a
descricao de ABI acima para saber o que pode alocar.

## O que invalidaria esta decisao

- Se as vinte operacoes do macro assembler nao bastarem para uma arquitetura e
  ela precisar reescrever o compilador de template mesmo assim. Ai o seam esta
  no lugar errado e desce um nivel.
- Se o codigo do tier 1 for medido como lento o bastante para importar, o que
  seria surpreendente: ele existe para coletar perfil e para ser alvo de deopt,
  nao para ser rapido.
