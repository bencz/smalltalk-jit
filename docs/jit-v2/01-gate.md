# O gate crescente

Sob corte seco (ADR 0002) a suite nao fica verde a cada commit, entao ela nao
pode ser o contrato de corretude. Este documento e o contrato.

Uma escada de niveis. Cada nivel tem **um comando** e **um criterio objetivo**.
O nivel atual fica em `scripts/gate.level`. Um commit e valido quando
`scripts/gate.sh` passa, e `gate.sh` roda **todos os niveis ate o atual**, nao
so o ultimo. **O nivel so sobe.** Descer exige apagar a linha do registro abaixo
e escrever por que, no commit.

## A escada

| n | nome | criterio |
|---|---|---|
| 0 | aloca e coleta | self-test em C do subsistema de memoria, linkado SOZINHO |
| 1 | troca de fiber | self-test em C do subsistema de fibers, tambem sozinho |
| 2 | objetos e colecoes | String, Symbol, Array, OrderedCollection sob coleta |
| 3 | JIT executa | bytecode escrito a mao, compilado e EXECUTADO, com perfil de tipos |
| 4 | constroi SSA | bytecode vira SSA, com estado de deopt anexado |
| 5 | otimiza | os passes, incluindo promocao de representacao de phi |
| 6 | escapa e materializa | objeto apagado e RECONSTRUIDO a partir da receita |
| 7 | primitivas | aritmetica por SEND, e o fallback quando a primitiva falha |
| 8 | front end | fonte Smalltalk vira bytecode, compila e RODA |
| 9 | compila | `cmake --build build` limpo, com `-Werror`, sem avisos |
| 10 | executa | `st -e '(3 + 4) printNl'` imprime `7` |
| 11 | bootstrap | `st -s snap -b packages/Core` completa e a imagem recarrega |
| 12 | pacotes | os quatro pacotes compilam e a imagem do projeto e gerada |
| 13 | paridade | `run_tests.sh` verde, **158 ok**, TUDO |
| 14 | deopt-stress | a suite inteira verde com `ST_DEOPT_STRESS=1`, resultado identico |
| 15 | performance | `vec3_flat`: zero alocacao, zero unbox, zero send no laco |

Os niveis 0 a 8 sao subsistemas provados SOZINHOS, cada um linkado a mao com meia
duzia de arquivos e nenhum CMake. Isso e deliberado: eles tem que continuar
passando enquanto o resto do VM ainda nem compila, que e precisamente a janela
que o corte seco abriu, e memoria, fibers, objetos, JIT, SSA, otimizador,
materializacao, primitivas e front end sao justamente os subsistemas que nao
dependem de um VM inteiro para serem verificados.

Os niveis 9 a 13 sao o VM voltando a existir e alcancando PARIDADE COMPLETA com
o v1. O 14 e o que torna a especulacao confiavel. O 15 e o alvo de performance.

### Os niveis 7 e 8 foram INSERIDOS, e o que isso custou

A escada original ia de "escapa e materializa" direto para "compila". Dois
subsistemas faltavam ali, e os dois pela mesma razao: a fase 0 os tratou como
itens de inventario e nao como coisas que dava para provar sozinhas.

**7, primitivas.** `3 + 4` compilava para um SEND com **nada do outro lado**.
Nenhum programa real roda assim.

**8, front end.** Tudo abaixo dele foi provado com bytecode escrito a mao. O
nivel 8 e onde o bytecode passa a vir de FONTE, e ele e provavel sozinho pelo
mesmo motivo que os outros: parser mais emissor mais JIT mais primitivas nao
precisam de um VM inteiro, precisam de um heap e de um kernel minimo.

Inserir os dois custou renumerar de 7 para cima. Nao apagou historia nenhuma: o
registro abaixo so tinha linhas ate o 6, e nenhum dos niveis deslocados havia
sido alcancado. Se algum tivesse, a insercao teria ido para o fim da escada em
vez do meio.

## O criterio de aceitacao e paridade, nao o benchmark

O alvo de `vec3_flat` (zero alocacao, zero unbox, zero send no laco) e o alvo de
PERFORMANCE, e ele e o ULTIMO nivel justamente porque nao e o criterio de
aceitacao.
O criterio de aceitacao e que **tudo funcione**, e "tudo" tem tamanho medido:

| | |
|---|---|
| arquivos em `tests/` | **141** |
| pacotes | **4**: Core, Std.Http, Std.Actors, Std.Uuid |
| fontes Smalltalk nesses pacotes | **160** |
| testes proprios dos pacotes | 7 |
| samples que a suite roda | 71 |
| self-tests em C | 3 (`ST_SMALLFLOAT_TEST`, `ST_BIGINT_TEST`, `ST_ABI_EMIT_TEST`) |
| **total de itens que `run_tests.sh` reporta** | **158** |

E nao e so rodar: o nivel 12 exige que os quatro pacotes COMPILEM e que a imagem
do projeto seja GERADA, porque `run_tests.sh` e `run_benchmarks.sh` fazem
bootstrap de imagem nova a partir de `packages/Core` a cada execucao. A imagem e
derivada, e e por isso que trocar o modelo de objeto foi possivel sem migracao;
a contrapartida e que gerar a imagem faz parte do que tem que funcionar.

Um JIT que faz `vec3_flat` chegar a zero alocacoes e nao roda `ExceptionTest.st`
nao entregou o projeto. A ordem dos niveis diz isso: **paridade primeiro,
performance depois**.

Os niveis 0 e 1 sao **linkados a mao**, cada um com meia duzia de arquivos e
nenhum CMake, e isso e deliberado: os dois tem que continuar passando enquanto
o resto do VM ainda nem compila, que e precisamente a janela que o corte seco
abriu. Memoria e fibers sao os dois unicos subsistemas que nao dependem de
motor de execucao nenhum, e por isso sao os dois unicos que dava para provar no
mesmo dia em que foram escritos.

## Por que "aloca e coleta" vem ANTES de "compila"

A primeira versao desta escada tinha "compila" no nivel 0, e isso estava errado
de um jeito que so apareceu ao rodar. Sob corte seco, compilar o tree inteiro e
o nivel MAIS DIFICIL, nao o mais facil: exige que o JIT ja exista, porque
metade do VM chama `generateMethodCode` e companhia. Com ele no nivel 0, o gate
ficaria vermelho por semanas e nao mediria nada durante todo esse tempo, que e
exatamente o que um gate nao pode fazer.

O subsistema de memoria, ao contrario, **nao depende de motor de execucao
nenhum**. Ele compila e se auto-testa sozinho no dia em que e escrito. Por isso
ele e o nivel 0: e a primeira coisa que da para provar, e prova-la antes de
construir qualquer coisa em cima e o unico momento barato de faze-lo.

O self-test do nivel 0 e linkado a mao, com sete arquivos e nenhum CMake, de
proposito: ele tem que continuar passando enquanto o resto do VM ainda nem
compila, que e precisamente a janela que o corte seco abriu.

## Por que deopt-stress vem antes de performance

Porque performance sem deopt-stress e uma mentira mensuravel. Zerar alocacao num laco e facil se voce
nao precisar reconstruir o estado quando a especulacao falhar. O `--deopt-stress`
e o que prova que da para desfazer, e sob corte seco ele e o **oraculo interno**
que substitui o oraculo externo que perdemos: com todo guard falhando, todo teste
e todo benchmark tem que produzir resultado identico ao do caminho normal.

## O oraculo externo, que continua disponivel

O corte seco tirou o v1 da branch, nao do repositorio. Para qualquer duvida de
"o v2 esta respondendo certo", master continua a poucos segundos de distancia:

```sh
git worktree add /tmp/st-master master
(cd /tmp/st-master && cmake -S . -B build && cmake --build build -j"$(nproc)")
(cd /tmp/st-master && ./build/st -s snapshot -b packages/Core)

# mesmo programa nos dois, saidas comparadas
diff <(/tmp/st-master/build/st -s /tmp/st-master/snapshot -f prog.st) \
     <(./build/st -s snapshot -f prog.st)
```

Isso nao ressuscita o v1 na branch e nao contamina o desenho novo: e uma
referencia externa, usada sob demanda. **A partir do nivel 3 esta e a maneira
certa de investigar qualquer resposta suspeita**, e e mais barata que ler codigo.

Limite conhecido: a partir do momento em que o modelo de objeto mudar (ADR 0003,
R4 a R6), o snapshot deixa de ser compativel entre os dois. Nao e problema:
`run_tests.sh` regera a imagem a partir de `packages/Core` a cada execucao nos
dois lados, entao o que se compara e a SAIDA do programa, nunca a imagem.

## Registro de niveis

Apendar, nunca reescrever. Data, commit, o que destravou.

| data | nivel | commit | nota |
|---|---|---|---|
| 2026-07-27 | - | `583047d` | baseline v1 antes do corte: **158 ok**, 3375 ms, 141 testes + 4 pacotes |
| 2026-07-27 | - | (nao commitado) | corte seco: 87 arquivos, 24.520 linhas fora |
| 2026-07-27 | **0** | (nao commitado) | memoria nova: 17 de 17 no self-test, sem motor de execucao |
| 2026-07-27 | **1** | (nao commitado) | fibers novos: 11 de 11; o self-test pegou `committedLow` sem `volatile` |
| 2026-07-27 | 0 | (nao commitado) | handles: 22 de 22; o self-test pegou o TLAB sobrevivendo a coleta |
| 2026-07-27 | **2** | (nao commitado) | camada de objetos: 23 de 23; pegou a tabela de simbolos sem write barrier |
| 2026-07-27 | **3** | (nao commitado) | JIT executa: 28 de 28; perfil de tipos e emissao cruzada entre duas ABIs |
| 2026-07-27 | **4** | (nao commitado) | SSA: 17 de 17; phis de back-edge, estado de deopt so com vivos |
| 2026-07-27 | **5** | (nao commitado) | otimizador: 16 de 16; corpo de laco sem conversao nenhuma |
| 2026-07-27 | **6** | (nao commitado) | escape + materializacao: 15 de 15 |
| 2026-07-27 | **7** | (nao commitado) | primitivas: 98 de 98; a escada ganhou um degrau, ver acima |
| 2026-07-27 | 0 | (nao commitado) | o nivel 7 achou DOIS bugs de GC pre-existentes, ver abaixo |
| 2026-07-27 | **8** | (nao commitado) | front end: 40 de 40; fonte -> bytecode -> maquina, sem bloco nao-inlinado ainda |
| 2026-07-27 | 8 | (nao commitado) | frames compilados viraram raizes: alocar de codigo compilado ficou seguro |
| 2026-07-27 | 7 | (nao commitado) | closures planas com celulas (ADR 0008) em bytecode: 104 de 104 |
| 2026-07-27 | 7 | (nao commitado) | nomes de primitiva alinhados a packages/ (173 declaradas, 31 implementadas): 109 de 109 |
| 2026-07-28 | 8 | (nao commitado) | closures no FRONT END: analise de captura e emissao de CLOSURE, 69 de 69 |
| 2026-07-28 | 0 | (nao commitado) | as closures acharam DOIS bugs de memoria pre-existentes, ver abaixo: 27 de 27 |
| 2026-07-28 | 8 | `c2ac221` | closures commitadas |
| 2026-07-28 | 8 | `fe9b5e8` | `super` no tier 1 e retorno nao local: 88 de 88 |
| 2026-07-28 | **9** | (nao commitado) | CMakeLists reescrito para o conjunto v2; `cmake --build` limpo com -Werror |
| 2026-07-28 | **10** | (nao commitado) | kernel EMBUTIDO em C + `st -e`: `(3 + 4) printNl` imprime 7 |
| 2026-07-28 | 10 | `c4ef859` | construtor de classes: 142 arquivos do Core, 1567 metodos, 16 falhas nomeadas |
| 2026-07-28 | 10 | `e77a111` | formas reconciliadas e os 5 arquivos v1 reescritos: o Core inteiro constroi |
| 2026-07-28 | 10 | (nao commitado) | o Core EXECUTA: 158 inicializadores, metaclasses eager, sends ate 5 argumentos |

## O que o nivel 7 encontrou, e por que so ele podia encontrar

Dois bugs de raiz de GC, os dois anteriores ao nivel 7 e os dois invisiveis para
todos os niveis abaixo dele. Ficam registrados porque a CAUSA de terem escapado
importa mais que os bugs.

**1. O scan de Cheney nao alcancava objeto PROMOVIDO** (`memory/Collector.c`). O
ponteiro de scan varre a to-space em ordem de endereco, e isso e a lista de
trabalho completa apenas para quem foi copiado PARA a to-space. Um objeto
promovido para a old space e sobrevivente igual, com slots igualmente obsoletos,
e nao esta naquela faixa. Os slots dele ficavam apontando para a semispace morta.

Por que nenhum nivel via: o nivel 0 promovia e conferia o objeto promovido, mas
nunca relia um PONTEIRO dele depois. E o sintoma nao aparece na coleta: o objeto
esta intacto, a coleta reporta sucesso, e o dado so vira lixo quando a semispace
for reusada, a uma distancia arbitraria do bug. O teste de regressao que ficou
(`vm/tests/MemoryTest.c`) confere a GERACAO do alvo, e nao o conteudo dele: com o
conserto removido, a checagem de conteudo **continua passando**, porque nada
chegou a encaminhar o objeto abandonado.

**2. `IcCell.selector` e `CodeUnit.literals` nao eram raizes** (`jit/Jit.c`). Um
`CodeUnit` e um struct C de `malloc` com `Value`s tagueados dentro, e uma celula
de cache guarda o seletor como ponteiro cru. Nenhum dos dois esta dentro de
objeto de heap, entao nenhum provedor de raiz os encontrava, e uma coleta jovem
move exatamente os objetos que eles nomeiam. Como lookup de seletor e por
IDENTIDADE de Symbol interned, o sintoma e `doesNotUnderstand` para um metodo que
existe, a partir da primeira coleta e nunca antes dela.

Conserto: `rootsVisitCompiledCode` em `memory/Roots.h`, o mesmo seam de
`rootsVisitNativeFrames`, com no-op fraco para quem linka sem JIT.

**A licao operacional**: os dois so apareceram quando um teste passou a
**coletar e continuar executando**. Todo nivel ate o 6 ou coletava no fim, ou
executava sem coletar. Nivel novo que envolva heap deve fazer as duas coisas, e
nessa ordem.

## O que o nivel 8 encontrou

**Um bug de precedencia no parser**, que sobreviveu ao v1 inteiro
(`vm/compiler/Parser.c`). As macros de checagem de token nao parentizavam o
parametro:

```c
if ((token->type & tokenType) == 0) { ... }
```

Os chamadores passam um CONJUNTO de alternativas, `TOKEN_IDENTIFIER |
TOKEN_KEYWORD | ...`, e `&` liga mais forte que `|`, entao o teste lia
`(type & PRIMEIRO) | RESTO`. Como RESTO e uma constante nao-zero, a condicao era
sempre falsa e **a checagem aceitava qualquer token**, em todos os lugares onde
uma alternativa era passada. Nunca reportou erro de sintaxe nesses pontos.

Achado por `-Wparentheses`, nao por teste, e nao poderia ter sido achado por
teste com facilidade: o efeito e aceitacao silenciosa, entao so um programa
MALFORMADO que mesmo assim compila o revela.

## Frames compilados viraram raizes, e o que quase deu errado

`rootsVisitNativeFrames` era o no-op fraco de `memory/Roots.c`, entao NADA podia
alocar com um frame compilado vivo. Isso bloqueava closures (que alocam),
`Object new`, e todo o resto. Agora esta implementado, e duas coisas o tornaram
barato:

**O mapa de frame do tier 1 e UNIFORME.** Todo slot de um frame de template
guarda um `Value` tagueado, sempre: o compilador de template nunca poe double cru
nem inteiro cru num slot, e o prologo nilifica todo slot que nao recebe
argumento. Entao o mapa e "slots 0 a frameSlots-1, todos ponteiros" e **nao
existe tabela por safepoint neste tier**. Isso muda no tier 2, onde o backend de
SSA guarda valores crus e o R1 do ADR 0003 passa a valer de verdade.

**A ancora custa ZERO instrucao no codigo gerado.** O frame pointer sai do
endereco de slot que a chamada ja passa, e o metodo sai de
`__builtin_return_address(0)`. Nada foi emitido para isso.

**O erro que quase passou**: ancorar so o segmento MAIS NOVO. Frames compilados
vem em SEGMENTOS separados por frames C:

```
churn (compilado) -> jitDispatch (C) -> new: (compilado) -> primitiva (C)
```

Com uma ancora so, o walk achava o frame de `new:`, parava na fronteira C, e os
registradores vivos de `churn` nunca eram varridos. As ancoras sao uma CADEIA.

**E a licao do teste, que e a mesma de antes**: a checagem tem que ser a
GERACAO do objeto, nao o conteudo dele. Um objeto que o coletor nunca viu fica
abandonado na semispace evacuada com os bytes intactos, entao "ainda le 111"
responde SIM mesmo com o walk desligado. Medido: a primeira versao da checagem
passava com o walk inteiro removido. O que nao acontece por sorte e estar na OLD
space, porque so um coletor que ACHOU o objeto duas vezes poderia te-lo
promovido.

## Closures: FEITAS, e o que decidiu o desenho

O ADR 0008 esta implementado dos dois lados. **Em bytecode**, verificado no
nivel 7: `CLOSURE`, `GETUP`, `NEWCELL`, `GETCELL`, `SETCELL`, mais
`value`/`value:` como primitivas (que sao SENDs, entao um sitio de chamada de
bloco carrega cache inline como qualquer outro). Objeto `Closure` = contagem,
metodo, capturas, tudo tagueado (`FORMAT_INDEXED_POINTERS`), entao o coletor nao
tem caso especial.

Provado com bytecode escrito a mao, de proposito, pelo mesmo motivo dos niveis
abaixo: o mecanismo erra de jeitos que o nivel de fonte esconde.

**E no FRONT END**, verificado no nivel 8: analise de captura mais emissao de
`CLOSURE`. Tres coisas decidiram o desenho.

**UM predicado de inlining, lido pelos dois lados.** A analise e o emissor
chamam a MESMA funcao para decidir se um bloco vira controle de fluxo ou
closure. Se discordassem, a discordancia seria silenciosa e cara: um
`sum := sum + i` dentro de um `to:do:` cujo bloco a analise achasse ser closure
poria `sum` numa celula de heap, e o resultado e um programa correto com o laco
morto, que e exatamente o laco que este projeto existe para otimizar. O nivel 8
le o bytecode de volta e conta: o laco inlinado tem ZERO instrucoes de celula e
ZERO `CLOSURE`, e o mesmo laco com uma closure lendo o acumulador tem as duas
coisas. Um numero so nao provaria nada; sao os dois que provam.

**A analise roda ANTES de existir uma instrucao.** Celula ou registrador muda
toda leitura e toda escrita da variavel, inclusive as emitidas antes da closure
que a captura. Decidir no sitio da closure exigiria reescrever o que ja foi
emitido.

**As tabelas da analise sao colecoes de HEAP, nao arrays C de `RawObject *`.**
A emissao aloca, entao um array C de ponteiros para nos da AST seria um conjunto
de enderecos que a primeira coleta invalida. Essa e a familia de bug que este
repositorio ja pagou tres vezes; nao vai pagar a quarta dentro do compilador.

Onde a celula e criada e onde o registrador e inicializado, e isso da de graca
as duas semanticas que importam: temporario de bloco inlinado ganha celula NOVA
por iteracao (closures feitas num laco capturam variaveis diferentes) e
temporario de metodo ganha UMA para a ativacao inteira (closures irmas
compartilham).

Tres guardas sao ASSERT e nao teste, porque o modo de falha e silencio: um bloco
que a analise nao viu, uma escrita em captura por valor, e "todo bloco preparado
foi emitido exatamente uma vez", que e a unica maneira de tornar barulhenta a
divergencia na direcao contraria.

## `super`, e onde a busca comeca

`SENDSUPER` roda no tier 1. O conteudo inteiro da palavra e UM argumento
diferente no caminho de send: a busca comeca onde o COMPILADOR disse, na classe
acima da que DEFINIU o metodo em execucao, e nao em nada que o receptor nomeie.

Nao da para derivar em runtime. O receptor pode ser instancia de uma subclasse
tres niveis abaixo, e comecar pela classe dele acharia o proprio metodo em
execucao de novo, que e recursao infinita. Por isso o indice da classe inicial e
resolvido na COMPILACAO e mora na celula do sitio (`IcCell.lookupStart`): indice
e estavel e nao precisa de relocacao (ADR 0005), a sequencia de chamada ja
materializa o endereco da celula, e o sitio continua sendo um send comum com
perfil comum. O teste que separa uma implementacao certa de uma errada e o de
RECEPTOR DE SUBCLASSE; com a versao errada ele responde 2 em vez de 1.

Super numa classe sem superclasse tem `lookupStart` invalido e vai para
doesNotUnderstand, em vez de recomecar pelo receptor, que e a mesma recursao.
Super sem classe definidora nenhuma e **erro limpo** de compilacao.

## Retorno nao local: o token e o registro

`^` dentro de bloco retorna do METODO EM QUE O BLOCO FOI ESCRITO, quantas
ativacoes houver no meio, e entre o bloco e o home ha frames compilados E frames
C alternados. Os frames C sao o que descarta "vai desempilhando frame compilado".

Duas metades:

**O TOKEN responde QUAL ativacao.** E um contador, nao um endereco de frame,
porque endereco e reusado: um bloco que sobrevive ao home tem que achar NADA, e
um endereco acabaria casando com o frame de um estranho no mesmo lugar. A closure
carrega o token da ativacao em que nasceu, como SmallInteger tagueado, entao o
objeto continua uniformemente tagueado e o coletor continua sem caso especial.

**O REGISTRO responde COMO CHEGAR LA.** Mora no frame C que ENTROU na ativacao
home, entao a cadeia se desfaz sozinha conforme esses frames retornam, e guarda o
destino do salto mais o estado que um salto deixaria para tras (a cadeia de
ancoras de frame compilado e a de handle scopes: quem foi pulado nao passou pelo
proprio `leave`).

**QUEM PAGA.** So um metodo que de fato tem `^` dentro de bloco ganha registro, e
quem decide isso e o front end na compilacao (`CodeUnit.couldBeHome`). Programa
que nunca escreve um roda exatamente como antes: token nenhum e cunhado, registro
nenhum e empilhado, e NADA e emitido em send nenhum. Essa e a razao de o custo
ficar aqui e nao como uma checagem depois de cada send, que e o outro jeito de
construir isso e taxaria o caminho mais quente do sistema por causa de um recurso
que a maioria dos sends nao usa.

Tres checagens do nivel 8 sao as que separam desenho certo de desenho parecido, e
as tres foram verificadas DESLIGANDO o conserto:

- **recursao**: cada ativacao do mesmo metodo cunha o proprio token, entao cada
  bloco retorna da ativacao que o construiu, e nao da mais externa;
- **bloco dentro de bloco**: o de dentro herda o home do de fora, e nao a ativacao
  sob a qual esta rodando. Com uma home intermediaria empilhada de proposito, a
  versao errada responde 0 em vez de 14;
- **home ja retornado**: token aposentado nao casa com registro nenhum, entao
  levanta. Roda em processo FILHO, porque levantar hoje e abortar (nao ha
  excecoes no v2 ainda), e o teste confere COMO o filho morreu.

## O que as closures encontraram no subsistema de memoria

Dois bugs pre-existentes, os dois no nivel 0, os dois invisiveis para tudo o que
rodou antes. Ficam registrados porque a CAUSA de terem escapado importa mais que
os bugs.

**1. Objeto INDEXADO com slot NOMEADO era medido pela contagem de elementos.**
Um `Closure` e exatamente isso: contagem, depois o metodo, depois as capturas. A
contagem de elementos nao sabe do slot nomeado, entao `objectSizeInBytes`
respondia UMA PALAVRA A MENOS: o walk entrava no meio do objeto, e a faixa de
ponteiros calculada parava ANTES do campo `method`. Ninguem atualizava esse
campo, e o primeiro scavenge que movesse o `CompiledMethod` deixava a closure
apontando para um cadaver.

Por que o nivel 7 nao viu: closure la so passou por `collectorMarkSweep`, que
**nao move**. O ponteiro obsoleto continuava certo por acidente.

Conserto: o CABECALHO responde primeiro. `sizeWords` e o tamanho TOTAL e foi
escrito por `objectSizeForShape`, entao ja contempla o que vem antes dos
elementos. A derivacao pela contagem fica so para o objeto grande demais para o
campo, e um `ASSERT` em `objectSizeForShape` mantem slot nomeado fora desse
caminho. O teto que sai dai e `CLOSURE_MAX_CAPTURES`, com **erro limpo** de
compilacao.

**2. O alocador nao escrevia o corpo, e as semispaces nao sao rezeradas.** O
comentario dizia "o corpo ja esta zerado", e isso e verdade so no primeiro ciclo:
a partir da segunda troca de semispace uma alocacao cai sobre os BYTES DE UM
OBJETO MORTO. Todo slot que o alocador nao escrevesse guardava o valor do
cadaver, e o coletor varre esse slot. O PADDING de alinhamento e o caso facil de
perder: um objeto de dois slots ocupa tres palavras de corpo, a faixa de
ponteiros sai do TAMANHO, e ninguem escreve a terceira.

Conserto: `initializeObject` escreve o corpo inteiro. Com ZERO, que e
`tagInt(0)`, e nao com nil: no VM inteiro um slot nao setado responde falso a
`valueTypeOf(slot, VALUE_POINTER)` e significa AUSENTE (classe sem superclasse,
metodo sem dono), e nil e um objeto. O que o Smalltalk exige, `Object new` com
variaveis em nil, passou a ser feito na primitiva que serve `new`, que e onde a
regra do Smalltalk comeca a valer.

**Os dois testes checam GERACAO, nao conteudo**, e os dois foram verificados
DESLIGANDO o conserto: o primeiro vira FAIL limpo, o segundo tambem, e a
ausencia do registro de unidades (abaixo) vira SEGV dentro do coletor.

## Um quarto baked pointer, e por que a unidade de bloco e diferente

A unidade de um bloco e construida quando o metodo que a contem compila, e nao
roda ate alguem mandar `value` para a closure. Nesse intervalo o frame de
literais dela nao era alcancavel de lugar nenhum: uma `CodeUnit` e um struct C
de `malloc`, e o `CompiledMethod` que a segura a segura como PALAVRA CRUA que o
coletor nao segue.

`jitRegisterUnit` passou a registrar a unidade quando ela e CONSTRUIDA e nao
quando e compilada. Com o registro desligado, o nivel 8 nao falha: ele SEGFATA
dentro do `markSlot`, seguindo o ponteiro obsoleto.

## Um baked pointer que o coletor movia

O terceiro da mesma familia (celula de IC, literal de CodeUnit, e agora este), e o
mais barato de errar: **`nil`, `true` e `false` sao BAKEADOS como imediatos no
codigo gerado.** `ifTrue:` inlinado e um compare contra o singleton `true`, e o
prologo enche os slots nao usados com `nil`, e as duas coisas sao uma instrucao
so porque o endereco e constante.

So que os tres eram alocados na NURSERY, entao a primeira coleta os movia e todo
compare bakeado parava de casar. O sintoma: um metodo que recebe `false` nao
entra em nenhum dos dois bracos e cai no caminho `mustBeBoolean`, para um valor
que E `false`. Nada quebra ate a primeira coleta, e o programa que revelou isso
alocava 64 MB num laco.

**Conserto**: `allocateImmortalObject` poe os tres na old space, que nao move
(ADR 0005), entao o endereco e permanente e bakear passa a ser legitimo.

**A guarda e um ASSERT, nao um teste**: `jitCompileFor` confere em TODA
compilacao que os tres sao `isOldObject`. Vale mais que teste porque roda em toda
compilacao de toda execucao da suite, e porque o modo de falha e silencio. Ele
imediatamente pegou o bootstrap do nivel 3, que ainda alocava `nil` na nursery.

## O contrato REAL esta em packages/, e ele agora e o que o VM fala

Achado tarde e registrado antes de virar retrabalho maior.

`packages/Core/src/*.st` declara primitivas por PRAGMA NOMEADA, e sao **173
nomes distintos** em uso:

```smalltalk
+ aNumber [
	<primitive: IntAddPrimitive>
	...fallback em Smalltalk...
]
```

FEITO. `vm/runtime/Primitives.def` foi **extraido de packages/**, nao inventado:
as 173 declaradas, cada uma com o nome que o kernel escreve. E um X-macro
incluido duas vezes, uma para o enum e outra para a tabela, entao nome e
implementacao nao tem como divergir. O front end le a pragma.

Tres regras que sairam disso:

- **nome desconhecido e ERRO de compilacao** (e typo, ou primitiva que ninguem
  escreveu). **Nome conhecido sem implementacao NAO e erro**: o metodo compila, a
  tentativa de primitiva simplesmente nao e emitida, e ele roda o fallback
  Smalltalk que ja carrega. As duas situacoes sao indistinguiveis de fora e
  tratar iguais seria esconder o typo;
- por isso o `.def` e tambem o **checklist de paridade**: hoje **31 de 173**, e
  `primitiveCoverage()` responde isso em runtime em vez de alguem contar NULLs;
- `IntAddPrimitive` e `FloatAddPrimitive` apontam para a MESMA funcao. E de graca
  e e exatamente o caminho rapido de aritmetica mista que o v1 nao tinha e pagava
  100x.

Uma consequencia de desenho: o kernel declara so `<` e `=` como primitiva de
comparacao, e deriva `>`, `<=` e `>=` em Smalltalk (Magnitude). Os testes
passaram a fazer o mesmo, entao o laco de `to:do:` agora exercita despacho de
verdade e nao um atalho em C.

Duas divergencias de desenho que a leitura revelou, e que nao sao renomeacao:

1. **`AtPrimitive` e UMA primitiva polimorfica em `Object`**, herdada por String,
   ByteArray, Array e FloatArray. RESOLVIDO do jeito do kernel: uma so, que
   decide pelo FORMATO. O unico caso que o formato nao resolve e String contra
   ByteArray (os dois sao `FORMAT_BYTES` e `at:` responde Character para um e
   SmallInteger para o outro), e esse desempata por CLASSE. E o unico lugar deste
   arquivo onde o formato nao basta, e esta dito la.
2. **`Block.st` ainda e o desenho de CONTEXTOS do v1**: `<shape: BlockShape>` com
   `| compiledBlock receiver outerContext homeContext |`. O ADR 0008 substitui
   isso por closure plana, entao esse arquivo muda. O ADR ja aceitou a mudanca de
   comportamento observavel; o arquivo simplesmente ainda nao acompanhou.

Ha tambem pragmas de CLASSE (`<shape: BytesShape>`, `<shape: IndexedShape>`, ...)
que o bootstrap novo vai ter que ler para carimbar a forma da instancia.

## Niveis 9 e 10: o VM voltou a existir como PROGRAMA

**O que compila.** O CMakeLists foi reescrito para o conjunto v2 e ficou MENOR de
proposito. O corte seco tirou 87 arquivos; o que ainda nao foi reimplementado
NAO esta na lista, em vez de estar listado e quebrado. Continuam no disco e fora
do build, cada um voltando quando a camada dele voltar:
`core/{CompiledCode,Entry,Exception,Lookup,Namespace,StackFrame}.c`,
`compiler/{Compiler,Scope}.c`, `concurrency/Scheduler.c`,
`runtime/{Base64,FileSystem,Iterator,Json,Message,Primitives,Socket,Stream}.c`,
`tools/{Repl,Snapshot}.c`. Os backends POWER falham no CONFIGURE com mensagem
propria: nao foram portados para o macro assembler novo, e um build que anda
quase todo e depois nao acha um simbolo nao explica nada.

**O kernel EMBUTIDO, e por que ele existe.** Ha um problema de ordem: o kernel
de verdade e `packages/Core`, que e fonte Smalltalk, e ler fonte Smalltalk exige
parser, compilador, heap, classes para compilar contra e primitivas para mandar
mensagem. Alguma coisa tem que existir antes de qualquer imagem, e
`vm/tools/Bootstrap.c` e essa coisa: a classe das classes, as classes que o
proprio parser instancia, as classes dos imediatos, e as primitivas ja
implementadas. E andaime, nao um subconjunto de `packages/Core` que tenha que
ficar em sincronia com ele.

Duas decisoes dentro dele:

- **o fallback de uma primitiva embutida MANDA `#primitiveFailed`**, que ninguem
  implementa, entao a falha para e diz quem falhou. Responder nil viraria
  resposta errada em outro lugar: uma soma que estourou nao tem LargeInteger para
  onde cair aqui, e um `at:` fora de faixa nao tem excecao para levantar, porque
  nenhum dos dois existe antes de `packages/Core`;
- **as comparacoes derivadas sao compiladas de FONTE no bootstrap**
  (`> aNumber [ ^aNumber < self ]` e companhia), entao um bootstrap que passou
  daqui ja exercitou parser, compilador e JIT antes de o primeiro programa rodar.

**Uma primitiva que `packages/` nao escreve.** `PrintValuePrimitive`, e o
`Primitives.def` diz isso na linha dela. O kernel embutido nao tem stream, nem
`printOn:`, nem Transcript; sem ela o VM roda um programa e nao tem como dizer o
que saiu. Ela nao e ligada em `packages/Core` de proposito: o `printNl` de la vai
por stream BUFFERIZADO, e uma primitiva C escrevendo direto no descritor
intercalaria com esse buffer.

**A LICAO DO `Assert.h`, de novo.** A mensagem de `doesNotUnderstand` chegava
como `doesNotUnderstand: ` e mais nada. O seletor era impresso com `printf`, ou
seja em stdout, que quando redirecionado e bufferizado por bloco, e `abort()` nao
faz flush. E exatamente como o abort do teto de 64KB no POWER embarcou silencioso.
Agora a mensagem inteira vai para stderr, com `fflush(NULL)` antes do abort, e
passou a nomear tambem a CLASSE do receptor: sem ela a mensagem diz o que nao foi
entendido e omite quem nao entendeu, que e a metade que diz onde olhar.

**O CLI continua inteiro.** O vocabulario vem de `vm/tools/Cli.h` sem mudanca
nenhuma: `new`, `build`, `run`, `test`, `repl`, `help`, mais `-e -f -s -b -h`.
Um port nao e motivo para mexer nisso. O que e estreito hoje e quanto de cada
comando da para CUMPRIR: cada um termina numa operacao que o corte tirou e que
ainda nao voltou (ler/escrever imagem, transformar diretorio de pacote em
classes, o laco do REPL), e cada um DIZ qual e a peca que falta e sai com codigo
nao-zero. Comando que finge trabalhar e pior que comando que recusa. Imagem
achada pela BUSCA e passada por cima com um aviso; imagem pedida com `-s` e
recusada, porque ai o chamador pediu aquela imagem.

## O construtor de classes, e a MEDICAO do que falta para packages/

`ClassNode` vira classe de verdade: forma, superclasse, variaveis de instancia e
cada metodo compilado dentro dela. Metaclasse criada no primeiro uso, com a
cadeia de metaclasses PARALELA a de classes, que e o que faz metodo de classe ser
herdado; sao 294 metodos de classe so no Core, entao nao era opcional.

Duas decisoes que o Core forcou, as duas de desenho e nao de conveniencia:

**Referencia adiantada a global.** Um nome com INICIAL MAIUSCULA que ainda nao
existe ganha a Association agora, com nil, e a definicao preenche a MESMA
Association depois. O kernel precisa disso e nao da para reordenar em volta:
`Object>>at:` levanta um `OutOfRangeError` definido trinta arquivos abaixo, entao
a ordem de carga teria que ser uma ordem total que nao existe. A regra e a
MAIUSCULA, que e a convencao do proprio Smalltalk; nome minusculo que nao resolve
continua sendo ERRO, porque aquilo e variavel escrita errada e nada la na frente
vai definir.

**Campo nao setado do AST e ZERO, nao nil.** Os acessores de `Ast.h` testavam
"nao e nil", e no v2 o alocador escreve ZERO de proposito (ausente e diferente de
"setado como nil"). Resultado: TODA classe do Core respondia que era um namespace.
Os acessores que podem ser perguntados sobre campo que o parser nao escreve
passaram a responder NULL.

### O numero

```
$ st -b packages/Core
142 files, 158 classes, 1727 methods
```

**O Core inteiro constroi.** Zero falhas. O que faltava eram 16 classes em tres
grupos, e cada grupo foi uma correcao de natureza diferente:

| grupo | o que era | o que resolveu |
|---|---|---|
| 5 formas em conflito | Class, BoxedFloat64, Character, Array, Symbol | **HERANCA DE FORMA** mais **variaveis de classe**, os dois abaixo |
| 5 formas que o VM possui | `CompiledCodeShape`, `BlockShape`, `ContextShape`, `ExceptionHandlerShape`, `UnwindHandlerShape` | os `.st` eram o DESENHO V1 e foram reescritos, ver abaixo |
| 5 subclasses em cascata | CompiledBlock, CompiledMethod, ContextCopy, MethodContext, BlockContext | sumiram junto |
| 1 lacuna de linguagem | `thisContext` em `Exception.st` | recusa nomeada: o materializador do ADR 0008 nao existe ainda |

**A FORMA E HERDADA** quando a classe nao declara uma. Era isso que quebrava
`Array := ArrayedCollection [ ]` e `Symbol := String [ ]`, que nao repetem a
pragma do pai. Errar aqui e caro na direcao pior: o coletor teria lido a
CONTAGEM DE ELEMENTOS de um Array como se fosse a primeira variavel de instancia.

**NOME MAIUSCULO NA SECAO DE VARIAVEIS E VARIAVEL DE CLASSE**, que e a convencao
do proprio Smalltalk, e o kernel depende dela: `Character` declara `| Table |` e
o preenche num metodo de classe, e as instancias de Character sao IMEDIATOS, sem
slot nenhum onde uma variavel de instancia pudesse morar. Uma variavel de classe
vira Association no dicionario da classe, que e exatamente o que uma global ja e,
entao o compilador le e escreve com as instrucoes que ja tinha; o que muda e so
onde a Association foi achada.

**O ESPELHO TEM QUE CONCORDAR COM O STRUCT.** `Behavior + Class` declaram, em
ordem, os 9 campos tagueados de `RawClass`, e `<shape: ClassShape>` e a classe
dizendo em voz alta que e espelho de um struct em vez de o construtor inferir
isso de uma lista de campos. O construtor CONFERE a contagem: acrescentar campo
de um lado sem o outro apareceria como metodo lendo o slot errado, que e resposta
errada e nao crash.

Um campo saiu do espelho: **`instanceShape`**. No v2 a forma e palavra RAW no
trailer da classe, justamente para o coletor nunca varre-la (ADR 0005), entao o
Smalltalk chega nela por primitiva e nao por campo.

### Os cinco arquivos v1, e o que cada um virou

- **`Block.st`**: era o desenho de CONTEXTOS (`| compiledBlock receiver
  outerContext homeContext |`). Virou closure plana do ADR 0008,
  `<shape: ClosureShape>`, sem campo nenhum: `receiver` e uma captura como outra
  qualquer, e os outros dois eram elos de uma cadeia que nao existe mais;
- **`CompiledCode.st`**: a palavra `header` empacotava contagem de argumentos,
  de temporarios e numero de primitiva. Isso mora na CodeUnit, que e struct C.
  Junto foi o DESASSEMBLADOR em Smalltalk, que decodificava o bytecode do v1
  instrucao por instrucao e nao descreve mais nenhuma instrucao que exista;
- **`CompiledMethod.st`**: ficou com os dois campos tagueados que o v2 tem
  (`selector`, `ownerClass`). `literals` foi para a CodeUnit e `descriptors`
  virou array ao lado do codigo nativo;
- **`Context.st`**: deixou de ter forma propria. Ativacao e frame nativo, e um
  Context e MATERIALIZADO de um quando alguem pede;
- **`ExceptionHandler.st` e `UnwindHandler.st`**: objetos comuns ate a maquinaria
  de excecao existir. A cadeia que o VM percorre e a de registros de unwind que o
  retorno nao local ja construiu, nao uma cadeia desses.

Onde um metodo perdeu o campo que lia, ele passou a `self notYetImplemented`, que
LEVANTA. Responder nil seria resposta plausivel vinda de um metodo que nao tem
como computar nada, e isso e resposta errada em outro lugar.

### O Core agora EXECUTA, e onde ele para

`st -b DIR -e CODE` constroi as classes do pacote e roda codigo contra elas no
mesmo processo. Nao precisa de imagem: e a maneira mais barata de descobrir o que
o kernel de verdade faz quando executa, e foi ela que achou tudo o que segue.

```
142 files, 158 classes, 1727 methods, 158 initializers
```

Quatro coisas que so apareceram ao RODAR, cada uma um erro de desenho e nao um
detalhe:

- **`initialize` de classe tinha que rodar.** Nada e opcional ali: e
  `ExternalStream class initialize` que faz Transcript ser um stream no descritor
  1, e sem ele o proprio `printNl` do kernel manda `nextPutAll:` para nil. O
  carregador roda os 158 na ordem do manifesto, que e a ordem em que as classes
  foram definidas, porque um inicializador rotineiramente usa uma classe
  definida antes;
- **metaclasse preguicosa quebra HERANCA de metodo de classe.** Criar a
  metaclasse so para quem declara metodo de classe parecia economia e era bug: a
  cadeia de metaclasses e o que faz metodo de classe ser herdado, entao `Array`,
  que nao declara nenhum, continuava instancia da classe-das-classes e a busca de
  `with:with:` nunca chegava na metaclasse de ArrayedCollection. Agora TODA classe
  tem a sua;
- **"e uma classe?" mudou de pergunta.** Era "e instancia da classe-das-classes",
  e isso deixou de ser verdade no instante em que metaclasses existiram: uma
  classe e instancia da SUA metaclasse. O teste desceu um nivel e aceita os dois,
  que e o certo, porque `Array new` e `Array class new` sao os dois sends a uma
  classe;
- **send de 3 a 5 argumentos** era `FAIL()` com um PENDING. O teto agora e o da
  ABI (receptor mais cinco, o conjunto de registradores inteiros do SysV), e
  passar disso DIZ isso em vez de chamar com argumentos que ninguem escreveu.

Onde para hoje: `3 + 4` roda contra o kernel de verdade, `7 printStringBase: 10`
tambem, e os bytes JA SAEM do processo (`ExternalStream write:next:from:` escreve
no descritor). `printNl` ainda morre acima disso, dentro da pilha de streams. Sao
**38 das 175 primitivas implementadas**, e os testes usam muito mais.

Duas armadilhas achadas ali, as duas da mesma familia:

- **`flush` chamando o SO era erro.** `fsync` num terminal ou num pipe responde
  EINVAL, entao `Transcript flush` falhava, o fallback levantava um IoError, e
  reportar esse IoError precisava do Transcript. Recursao infinita, medida. Nada
  e bufferizado do lado C; o buffer que existe e o do proprio kernel, em
  Smalltalk, e ja foi escrito quando esse send chega. E no-op de proposito;
- **metodo com primitiva e SEM corpo de fallback responde SELF.** `IoError class
  last` era exatamente isso, entao respondia a CLASSE IoError, e
  `IoError last signal` saia procurando um signal de classe. Vale como regra
  geral: primitiva nao implementada mais corpo vazio e um metodo que responde o
  receptor, silenciosamente.

E compilar nao e rodar. Entre isto e `run_tests.sh` verde estao, em ordem:
a IMAGEM (Snapshot le e escreve o modelo novo), as EXCECOES (`signal`, `on:do:`,
`ensure:`, que e o segundo cliente do unwind que o retorno nao local ja
construiu), e as PRIMITIVAS: 31 das 174 estao implementadas, e os testes usam
muito mais que isso.

## O que o gate NAO cobre, e como cobrimos

O gate mede **funcionamento**, nao **desempenho**, e no nivel 8 mede um alvo
unico. Duas coisas ficam de fora e precisam de tratamento proprio:

**Regressao de desempenho.** Fica com `scripts/ab.sh` mais
`benchmarks/results/BASELINE.jsonl`, exatamente como hoje. Regra herdada e nao
negociada: layout de codigo neste VM ja produziu 4% de variacao com instrucoes
identicas, entao nenhum ganho abaixo disso e reivindicavel sem A/B intercalado, e
a contagem de instrucoes retiradas manda sobre o relogio.

**Corretude que nenhum teste alcanca.** A licao registrada deste repositorio e
que um slot de stackmap faltando nao produz teste vermelho. A guarda certa nao e
teste, e `ASSERT` no codegen, e esse e o requisito R2 do ADR 0003. Todo `ASSERT`
novo que codifique uma invariante do mapa de deopt ou do stackmap vale mais que
um teste, porque roda em toda compilacao de toda execucao da suite.
