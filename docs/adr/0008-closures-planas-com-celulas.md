# ADR 0008: closures planas com celulas, nao contextos

Data: 2026-07-27. Status: aceito.

## Decisao

Um bloco captura variaveis externas por **valor**, dentro da propria closure,
exceto quando a variavel e **mutada depois da captura**: essa ganha uma
**celula** no heap e a closure captura o ponteiro da celula.

Nao ha cadeia de contextos e nao ha caminhamento de niveis. `GETUP i` e um
acesso direto ao i-esimo valor capturado; `GETCELL` e um load.

## Alternativas consideradas

**Contextos**, como o VM anterior: cada ativacao que precisa sobreviver aloca um
Context no heap, e um bloco alcanca as variaveis externas caminhando niveis
(`OPERAND_CONTEXT_VAR index level`).

**Celulas com Context materializavel desde o inicio**: o mesmo desenho de
celulas, mas com a reconstrucao de Context ja projetada em vez de deixada para
depois.

## Criterio que decidiu

**A analise de escape precisa de uma alocacao EXPLICITA para poder apaga-la.**
Uma celula e `NEWCELL`, uma instrucao, com um valor SSA de resultado e usos
rastreaveis: a fase 5 escalariza exatamente como escalariza qualquer outra
alocacao, e a receita de materializacao para a desotimizacao ja existe pelo
mesmo mecanismo. Um Context era opaco, e por isso a alocacao de ativacao nunca
sumia.

Os outros dois ganhos vem junto:

- **caminhar niveis desaparece.** Acesso a variavel capturada vira um load, nao
  um laco sobre a cadeia de contextos;
- **a maioria dos blocos nao aloca nada.** Um bloco que so LE variaveis
  externas captura por valor e nao toca o heap. Nos alvos do projeto (o corpo
  de `to:do:`, o corpo de `whileTrue:`) e o caso comum.

## O preco, aceito explicitamente

`thisContext` e a reflexao sobre ativacoes (`Context>>parent`, `receiver`,
`temporaryAt:`, o debugger) deixam de ser gratuitas. Passam a ser uma operacao
que **materializa** um Context a partir do frame nativo, usando o mesmo mapa de
slots tipados que a desotimizacao usa (`memory/Roots.h`), e um metodo que a
utiliza pode precisar ser desotimizado para responder.

Isso e **mudanca de comportamento observavel**, nao so de implementacao, e esta
sendo aceita de olhos abertos. E o que VMs modernas fazem, e o mecanismo que a
sustenta e o mesmo que a fase 3 constroi de qualquer forma, entao o custo
incremental e o de conectar e nao o de inventar.

## Consequencia para o retorno nao local

`RETOUTER` (o `^` dentro de um bloco, que retorna do metodo home) precisava da
cadeia de contextos para saber ate onde desenrolar. Com closures planas, a
closure passa a carregar um **token do frame home**, e o desenrolamento compara
tokens. O token tem que ser valido enquanto o frame home vive e invalido depois,
para que um retorno nao local para um metodo ja retornado levante em vez de
saltar para lixo.

## O que invalidaria esta decisao

- Se a materializacao de Context se mostrar necessaria em caminho QUENTE, e nao
  so em depuracao e reflexao. Ai o custo deixa de ser pago onde e barato.
- Se a analise de escape, uma vez medida, nao apagar celulas nos alvos. O ganho
  principal seria imaginario, e sobrariam so o acesso direto e a captura por
  valor, que continuam valendo mas nao justificariam sozinhos a mudanca de
  comportamento do `thisContext`.
