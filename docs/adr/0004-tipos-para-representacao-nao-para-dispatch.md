# ADR 0004: tipos servem para representacao e layout, nao para dispatch

Data: 2026-07-27. Status: aceito.

## Decisao

O jit-v2 nao tem sistema de tipos estatico. Nao ha anotacao de tipo em
parametro de metodo, nem checagem, nem inferencia. O que existe de "tipo" e:

1. **perfil de tipos em runtime**, coletado pelos caches inline (fase 2);
2. **representacao como parte do valor SSA**: TAGGED, F64, I64 ou BOOL, com
   `box_f`/`unbox_f`/`box_i`/`unbox_i` como instrucoes de primeira classe
   (fase 4);
3. **propagacao de tipos** como passe do otimizador (fase 5, passe 2);
4. **campos tipados nas classes de valor**, `| x::Float64 y::Float64 |`, usados
   EXCLUSIVAMENTE para decidir layout de memoria (fase 7).

O item 4 e a unica sintaxe nova, e ela nao participa de dispatch.

## Alternativas consideradas

**Tipos declarados opcionais em metodos** (`foo: x::Float64 ^Float64`), como
dica de representacao e nao como checagem: permitiria escolher representacao sem
esperar o perfil aquecer e passar f64 cru entre metodos sem box na fronteira.

**Sistema de tipos estatico completo**: anotacao, checagem, inferencia, erro de
compilacao. Mudaria `Parser.c` e `Compiler.c`, que neste projeto ficariam
intocados, e mudaria a semantica da linguagem.

## Criterio que decidiu

Um censo ja executado neste repositorio, registrado em
`docs/type-annotation-measurement.md` e instrumentado por `TypeStats`
(`vm/jit/Tier.h:88-117`). Sobre os sitios de send em metodos que alcancaram
tier 1, ou seja os quentes:

- **78% dos sitios anotaveis que executam ja estao MONOMORFICOS**, portanto ja
  foram devirtualizados pelo tier (chamada direta com guard, ou corpo inlinado).
  Uma anotacao ali compraria a remocao de um guard de 2 a 5 instrucoes, nao a
  remocao de um dispatch;
- sitios com PIC veem varias classes de fato, entao um tipo exato declarado ali
  seria uma mentira que levanta;
- so os buckets frio e megamorfico sao lugares onde a declaracao diz ao
  compilador algo que o runtime nunca aprendeu.

O saldo medido foi **cerca de 1% dos sends quentes**. A tese de performance da
anotacao para dispatch caiu com numero na mesa, e nao vai ser relitigada.

O mesmo desenho aparece no prototipo de referencia (`python_proto/README.md`):
aritmetica, `at:`, `at:put:` e `size` vao como SEND normal justamente para que o
cache inline colete perfil, e "os tipos aqui servem para layout e
representacao, nao para dispatch". E a posicao do Strongtalk: mesmo com tipos
declarados disponiveis, o perfil do PIC e mais preciso na pratica para decidir
despacho e inlining.

Onde a declaracao ganha e onde o perfil nao alcanca: **layout de memoria**. O
perfil diz "este receptor e sempre um Vec3"; ele nao diz "os tres campos de Vec3
sao doubles crus contiguos e um array de 200 deles sao 600 doubles sem cabecalho
nem ponteiro". Isso e informacao de declaracao, e e exatamente o que a fase 7
usa.

## Consequencia operacional

A disciplina da fase 2 e a outra face desta decisao e nao e negociavel:
aritmetica, `at:`, `at:put:` e `size` NAO podem ser resolvidos estaticamente
pelo compilador nem pelo codegen. Tem que passar por send com cache inline. Se o
compilador trapaceia, o perfil deixa de existir, e como o ganho vem do perfil, a
trapaca custa mais do que economiza.

Isso ja e violado no v1 de duas formas medidas, e as duas saem: o caminho rapido
de aritmetica pula o IC quando acerta (`CodeGeneratorX64.c:1566-1574`, entao o
perfil dos sitios aritmeticos e o COMPLEMENTO do comportamento tipico), e
`compiledCodeResolveOperandClass` devirtualiza sends de receptor conhecido sem
guard e sem celula (`CodeGeneratorX64.c:1489-1542`).

## O que invalidaria esta decisao

- Se, depois da fase 5, medirmos que a fronteira de metodo (box de f64 ao
  retornar de um callee que nao foi inlinado) e um custo dominante nos alvos. A
  resposta correta ai NAO e um sistema de tipos, e sim uma convencao de chamada
  especializada por representacao, escolhida pelo perfil. Anotacao so entraria
  se essa convencao provasse ser insuficiente.
- Se o objetivo do projeto passar a incluir corretude ou ferramenta (refactoring,
  navegacao, erro em tempo de compilacao). Sao objetivos legitimos e outros, e
  exigiriam ADR proprio.
