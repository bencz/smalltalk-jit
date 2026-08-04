# ADR 0006: sistema de tipos, para representacao e nao para dispatch

Data: 2026-07-27. Status: aceito. **Substitui o ADR 0004.**

## O que mudou

O ADR 0004 decidiu que nao haveria sistema de tipos, so perfil em runtime,
representacao dentro do valor SSA e campos tipados nas classes de valor. A
decisao foi revertida: haver **tipos declarados** e parte do projeto, para
permitir otimizacoes mais agressivas.

A reversao esta registrada em vez de apagada porque o criterio que decidiu o
ADR 0004 continua valido no seu proprio dominio, e confundir os dois dominios e
como o projeto erraria de novo.

## O criterio antigo, e por que ele nao contradiz o novo

`docs/type-annotation-measurement.md` mediu que anotacao de tipo removeria
**dispatch** em cerca de 1% dos sends quentes, porque 78% dos sitios anotaveis
que executam ja sao monomorficos e o tier ja os devirtualizou.

Esse numero e sobre DISPATCH. Ele nao diz nada sobre REPRESENTACAO, e sao
perguntas diferentes:

- "que metodo chamar" o perfil responde melhor que a declaracao, porque mede o
  que acontece em vez de prometer o que deveria acontecer. Essa e a posicao do
  Strongtalk e continua sendo a nossa;
- "em que forma esse valor vive" o perfil responde MAL, porque a resposta tem
  que valer na FRONTEIRA, antes de haver perfil, e tem que valer para o valor
  que ainda nao existe. Um `f64` que atravessa uma chamada de metodo so fica
  cru se as duas pontas concordarem, e concordar exige declaracao ou
  especializacao.

O criterio de aceitacao do projeto vive no segundo dominio. Por isso a reversao
tem fundamento e o numero antigo nao a refuta.

## Decisao

Tipos declarados, opcionais, usados para escolher REPRESENTACAO e LAYOUT:

1. **campos de classe de valor**: `| x::Float64 y::Float64 z::Float64 |`,
   ja previsto na fase 7, decide layout plano;
2. **parametros e retorno de metodo**: decide a convencao de chamada, ou seja
   se um `f64` atravessa a fronteira cru ou boxed;
3. **variaveis locais**, quando declaradas, decidem a representacao do slot sem
   esperar o perfil aquecer.

Continua valendo do ADR 0004:

- **dispatch continua vindo do perfil**, nao da declaracao. Um tipo declarado
  nao autoriza chamada direta sem guard;
- **aritmetica, `at:`, `at:put:` e `size` continuam passando por send com cache
  inline.** Se o compilador resolver isso estaticamente o perfil deixa de
  existir, e o perfil e de onde vem o ganho de dispatch.

Ou seja: a declaracao entra no eixo de REPRESENTACAO e nao no de DESPACHO. Os
dois se encontram no otimizador, que ja sabe combinar as duas fontes porque a
representacao ja e parte do valor SSA.

## O que fica em aberto, e vira ADR proprio

- **sintaxe**, alem do `::` que a fase 7 ja usa;
- **checagem**: se um tipo declarado e verificado (e o que acontece quando
  falha: erro em compilacao, guard em runtime, ou deopt);
- **inferencia**: se um tipo nao declarado pode ser deduzido, e ate onde;
- **generalidade**: se o sistema conhece so escalares (`Float64`, `Int64`,
  `Boolean`) ou tambem classes.

Nenhuma dessas bloqueia o trabalho atual. O que bloqueia e a decisao acima, que
esta tomada: a representacao e um eixo declarado, e o dispatch nao e.

## O que invalidaria esta decisao

- Se, medindo, a convencao de chamada especializada por perfil (o callee ganha
  uma entrada alternativa que recebe `f64` cru, escolhida pelo IC do chamador)
  entregar o mesmo ganho que a declaracao. Ai a declaracao vira conveniencia de
  quem escreve e nao mecanismo de performance, e o custo de linguagem deixa de
  se pagar.
