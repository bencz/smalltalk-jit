# Desenho do bytecode

Aceito. O conjunto de opcodes e a entrada de tudo o que vem depois:
construcao de SSA, mapa de desotimizacao, frame de tier 1 e OSR, e por isso
foi desenhado antes de qualquer linha de emissor.

## O que este bytecode tem que servir

Quatro consumidores, e cada um impoe uma restricao dura:

1. **Construcao de SSA em uma passada** (Braun et al.). Exige registradores
   virtuais PLANOS: um registrador vira um valor SSA por definicao, sem
   interpretacao. E o que o bytecode antigo NAO tinha, porque um operando la
   podia ser `INST_VAR_OF` com nivel, e ai a construcao precisa entender o
   operando antes de poder numerar.
2. **Alvo de desotimizacao** (ADR 0001, rota B). Exige que todo ponto de
   retomada tenha um indice estavel e que o estado seja "registrador -> valor".
3. **Frame fixo de tier 1**. Exige que slot do frame = registrador do bytecode,
   sem remapeamento.
4. **Perfil real** (ADR 0006). Exige que aritmetica, `at:`, `at:put:` e `size`
   sejam SEND, nao instrucao.

## Registradores

Cada metodo declara `nregs` registradores virtuais, planos, numerados de 0:

```
r0            self
r1 .. rA      argumentos, na ordem
rA+1 ..       temporarios, na ordem de declaracao, depois temporarios gerados
```

**Convencao de chamada: os argumentos de um send ficam em registradores
CONSECUTIVOS.** `SEND` nomeia o registrador do receptor e a aridade; os
argumentos estao logo acima dele. O front end paga MOVEs para arrumar isso.

O que isso compra: `SEND` cabe em largura fixa. O que custa: MOVEs que o
otimizador tem que remover (e remove, sao phis triviais e copias). Achei o
cambio favoravel; e a mesma escolha que Lua e Dalvik fizeram.

## Codificacao: 8 bytes, largura fixa

```
 byte 0     opcode
 byte 1     aridade / flags
 bytes 2-3  a       uint16
 bytes 4-5  b       uint16
 bytes 6-7  c       uint16
```

Largura fixa nao e estetica, e o que torna tres coisas triviais:

- **bci = indice da instrucao**, e o mapa bci -> offset de maquina vira um
  ARRAY, nao uma busca. O VM antigo tinha `descriptorsAtNativePosition` fazendo
  busca linear sobre um Array de descritores;
- desvio para tras nao precisa reinterpretar a corrente;
- o decodificador do template JIT e um `switch` sobre um `uint64_t` carregado,
  sem cursor variavel.

Tetos: 65.536 registradores por metodo, 65.536 literais, 65.536 instrucoes por
metodo. Estourar qualquer um e **erro limpo**, nunca truncamento silencioso.

## Os opcodes

### Movimento e constantes

| | |
|---|---|
| `MOVE d, a` | `d := a` |
| `LOADK d, k` | `d := literals[k]` |
| `LOADI d, imm` | `d := SmallInteger imm`, imm de 16 bits com sinal |
| `LOADNIL d` / `LOADTRUE d` / `LOADFALSE d` | |

`LOADI` existe porque inteiro pequeno e o literal mais comum de longe e nao
merece uma entrada na tabela.

### Estado de instancia

| | |
|---|---|
| `GETIVAR d, obj, i` | `d :=` i-esima variavel de instancia de `obj` |
| `SETIVAR obj, i, a` | |

Acesso a variavel de instancia NAO e send em Smalltalk, entao e instrucao.

### Globais

| | |
|---|---|
| `GETGLOBAL d, k` | `literals[k]` e uma Association; le o valor |
| `SETGLOBAL k, a` | |

### Sends

| | |
|---|---|
| `SEND d, sel, base, n` | `d := base(base+1 .. base+n)`, seletor `literals[sel]` |
| `SENDSUPER d, sel, base, n` | busca comeca na superclasse do metodo |

**Todo SEND tem um sitio de cache inline**, indexado pelo bci. E dali que vem
todo o perfil da fase 2: contagem por classe do receptor E do primeiro
argumento.

### Controle

| | |
|---|---|
| `JUMP t` | |
| `JUMPFALSE a, t` | desvia se `a` for `false` |
| `JUMPTRUE a, t` | |
| `GUARDCLASS a, ci, t` | desvia se a classe de `a` nao for `ci` |
| `RET a` | |
| `RETOUTER a` | retorno nao local, para o metodo home do bloco |

`GUARDCLASS` serve dois usos que parecem diferentes e sao a mesma coisa: o
front end o emite para provar que o receptor de um `ifTrue:` inlinado e
Booleano, e o tier 1 o emite para uma especulacao de classe. Como o alvo e um
INDICE de classe e nao um ponteiro (ADR 0005), ele e um `cmp` de 32 bits.

### Blocos e captura

| | |
|---|---|
| `CLOSURE d, blk, base, n` | fecha `blocks[blk]` sobre `base .. base+n-1` |
| `GETUP d, i` | i-esimo valor capturado pela closure em execucao |
| `SETUP i, a` | |
| `NEWCELL d, a` | caixa para uma variavel capturada E mutada |
| `GETCELL d, c` / `SETCELL c, a` | |

**Esta e a decisao mais consequente do documento**, ver a secao propria abaixo.

### Alocacao

| | |
|---|---|
| `NEW d, k` | instancia de `literals[k]`, tamanho fixo |
| `NEWIDX d, k, n` | instancia indexada com `n` elementos |

Alocacao e instrucao e nao send porque a analise de escape precisa VER a
alocacao para poder apaga-la. Um `new` escondido atras de um send generico e
opaco, e a fase 5 nao teria o que escalarizar.

### Ponto de suspensao

| | |
|---|---|
| `SAFEPOINT` | ADR 0007 |

Emitido em toda back-edge de laco e na entrada de metodo. Carrega o mapa de
estado exato. E o unico opcode que existe por causa do GC e dos fibers, e ele
existe porque quando NAO existia o mapa tinha que ser over-aproximado.

## O que deliberadamente NAO esta aqui

**Aritmetica.** `+`, `-`, `*`, `<` e companhia sao SEND. Nao ha `ADD d, a, b`.

Isso e contraintuitivo e e o ponto do projeto. O VM antigo resolvia aritmetica
no call site com caminho rapido inline, e o efeito medido foi: quando o caminho
rapido ACERTA, ele pula o cache inline (`CodeGeneratorX64.c:1566-1574`), entao
o perfil daquele sitio fica vazio, e quando nao fica, descreve exatamente os
casos que FALHARAM. O perfil de aritmetica nao era impreciso, era invertido.

**`at:`, `at:put:`, `size`.** Mesma razao. Sao sends, com cache inline, e o
otimizador os rebaixa a `aload`/`vaload` DEPOIS que o perfil disser a classe.

**Tipos.** Nao ha opcode tipado, nao ha `ADDF`. Tipo declarado (ADR 0006) e
metadado do METODO (tabela registrador -> tipo declarado), lido pelo construtor
de IR. O fluxo de bytes fica sem tipo, e assim o sistema de tipos pode evoluir
sem mudar o conjunto de opcodes.

## A decisao das closures

O VM antigo usava **contextos**: cada ativacao que precisava sobreviver
alocava um Context no heap, e um bloco chegava nas variaveis externas
caminhando niveis (`OPERAND_CONTEXT_VAR index level`).

Proponho **closures planas com celulas**:

- variavel capturada e NAO mutada depois da captura: copiada por VALOR para
  dentro da closure;
- variavel capturada E mutada: ganha uma CELULA no heap, e a closure captura o
  ponteiro da celula.

Por que trocar:

1. **Some o caminhamento de niveis.** `GETUP i` e um acesso direto; `GETOUTER
   level, i` era um laco.
2. **A analise de escape ganha alvo.** Uma celula e uma alocacao explicita
   (`NEWCELL`), entao a fase 5 pode apaga-la quando a closure nao escapa. Um
   Context era opaco.
3. **A maioria das capturas nao precisa de nada.** Um bloco que so LE variaveis
   externas nao aloca coisa nenhuma.

O que custa:

- `thisContext` e a reflexao sobre ativacoes deixam de ser gratuitas. Passam a
  ser uma operacao que **materializa** um Context a partir do frame, e um metodo
  que a usa pode ter que ser desotimizado. Isso e aceitavel e e o que VMs
  modernas fazem, mas e uma mudanca de comportamento observavel;
- retorno nao local (`RETOUTER`) precisa que a closure carregue um token do
  frame home, para saber ate onde desenrolar.

## Como isto serve cada consumidor

**SSA em uma passada.** Registrador plano vira variavel do algoritmo de Braun
direto. `MOVE` some como phi trivial. `GETIVAR`/`SEND`/`NEW` viram nos.

**Desotimizacao.** O estado em um bci e `{registrador vivo -> valor SSA}`, e a
vivacidade sai de uma analise retroativa sobre este fluxo, exatamente como
`liveness()` no `lower.py` do prototipo. Largura fixa faz do bci um indice.

**Tier 1.** Slot do frame = registrador, sem remapeamento. E o que torna o
frame um invariante e o mapa de deopt escrivel.

**OSR.** Entrada num laco = um bci especifico com o frame reconstruido a partir
do mapa. Sem largura fixa isso exigiria uma busca.

## Decisoes fechadas

1. **Closures planas com celulas.** Aceito, ADR 0008. `thisContext` passa a ser
   materializacao a partir do frame, e pode desotimizar o metodo que o usa.
2. **Argumentos em registradores consecutivos.** Aceito: e o que faz `SEND`
   caber em largura fixa, ao preco de MOVEs que o otimizador remove como copias
   triviais.
3. **`ifTrue:`, `whileTrue:` e `to:do:` inlinados pelo front end**, com
   `GUARDCLASS` provando que o receptor e Booleano. O perfil ali nao informa
   nada que o compilador ja nao saiba, e sem inlinar nao existe laco para
   otimizar.
4. **Tetos de 16 bits** para registradores, literais e instrucoes por metodo,
   com **erro limpo** ao estourar. O VM antigo teve exatamente esta classe de
   problema com um teto de `uint8` que aliasava silenciosamente associacoes cujo
   indice passava de 255; o conserto de la e o precedente para o erro explicito
   de agora.
