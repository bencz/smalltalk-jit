# ADR 0003: requisitos do GC novo, e o que fica em aberto

Data: 2026-07-27. Status: aceito (requisitos). Algoritmo em aberto, ver "Adiado".

## Decisao

`vm/memory/` e reescrito com o desenho **repensado**, nao reimplementado igual.
Este ADR fixa os **requisitos** que qualquer desenho tem que satisfazer, porque
eles saem do criterio de aceitacao do projeto e nao da preferencia por um
coletor. A escolha do algoritmo e adiada, com criterio explicito, para o momento
em que houver numero para decidir.

## Requisitos, e de onde cada um vem

**R1. Stackmap distingue ponteiro, f64 cru e i64 cru.**
Da fase 3 e da fase 6: um double em slot de pilha nao pode ser varrido como
ponteiro. Hoje todo slot marcado e tratado como tagged
(`Scavenger.c:306-315`), e o unico filtro e `valueTypeOf(*value, VALUE_POINTER)`,
que um padrao de bits de double satisfaz por acidente. O gancho ja existe e vale
zero: `frameRawAreaSize` (`CodeGenerator.h:19`) ja participa do calculo do
stackmap nos dois backends.

**R2. O coletor NAO repara. Divergencia e `ASSERT`.**
Hoje `scavengeStackSlot` (`Scavenger.c:239-279`) reescreve para `nil`, em
silencio, todo slot marcado cujo conteudo falha em `plausibleObject`. Isso existe
porque o mapa vem de um linear scan cego a fluxo de controle que marca slots
ainda nao escritos. Com liveness sobre SSA o mapa fica exato e o reparo deixa de
ser necessario. **Sob corte seco (ADR 0002) este requisito e a principal defesa
que sobrou**: sem ele, um mapa errado nao produz teste vermelho, produz corrupcao
silenciosa.

**R3. Desotimizacao e ponto de GC de primeira classe.**
Materializar objetos eliminados pela analise de escape aloca, possivelmente
varios objetos de uma vez, com todo o estado de deopt vivo. O estado de deopt
inteiro (incluindo as receitas de materializacao e os valores que preenchem os
campos) tem que ser um conjunto de raizes que o coletor enxerga naquele momento.

**R4. Identidade de classe estavel entre coletas.**
Hoje toda celula de cache inline e zerada a cada scavenge
(`Scavenger.c:583`, `GarbageCollector.c:345`) porque a celula guarda um ENDERECO
de classe. Isso destroi o perfil que a fase 2 existe para acumular. Ou indice de
classe com tabela, ou classes nao-moventes. **Isto e a mesma decisao que o `cmp`
de 32 bits da fase 6**, e nao duas.

**R5. Objeto sem ponteiro nao e varrido.**
Da fase 7: um array plano de classe de valor com campos todos escalares nao
contem ponteiro nenhum. Exige um campo de formato alcancavel sem derreferenciar
a classe.

**R6. Tamanho do objeto sem derreferenciar a classe.**
Hoje `computeRawObjectSize` (`Object.h:206`) faz objeto -> classe -> shape, duas
cargas dependentes por objeto varrido. Formato no cabecalho elimina a cadeia.

**R7. Caminho rapido de alocacao inlineavel no codigo do JIT.**
Bump pointer mais teste de limite. O criterio de aceitacao e sobre ELIMINAR
alocacoes; as que sobrarem tem que ser baratas. Hoje ha TLAB por mutator
(`Heap.h`, `youngLock` so no refill), o que ja e a forma certa.

**R8. Preservar o que ja funciona e nao e negociavel.**
Safepoints stop-the-world multicore com varios workers por heap; isolates como
heaps separados; remembered set por thread consolidado no heap; exec space que
nunca move nem libera (frames em voo terminam no codigo antigo); `Object>>become:`
(`Primitives.c:275`), que restringe desenhos que dependem de endereco como
identidade; pilhas de fiber crescentes de 64KB.

**R9. Bit 3 do endereco separa young de old** (`Object.h:25-34`), ou o teste de
geracao e trocado explicitamente em todo lugar que o usa. Nao herdar isso por
acidente.

## Adiado: qual coletor

Candidatos: (a) geracional copiador na young mais mark-sweep na old, o desenho
atual reconstruido; (b) mark-region estilo Immix; (c) semispace puro sem
geracoes; (d) geracional copiador mais mark-compact na old.

**Criterio que vai decidir**, nesta ordem:

1. satisfaz R1 a R9 sem excecao;
2. taxa de alocacao medida dos alvos DEPOIS que a analise de escape agir. O
   ponto do projeto e que `vec3_flat` aloque zero; um coletor dimensionado para
   a taxa de alocacao de hoje pode estar otimizando um regime que vai deixar de
   existir. **Por isso a escolha vem depois da fase 1 e nao antes**;
3. complexidade do write barrier contra o ganho, medida com o barrier no lugar;
4. pausa sob os benchmarks multicore ja existentes.

## O que invalidaria estes requisitos

- **R4** cai se medirmos que perfil persistente nao muda decisao de inlining nos
  alvos. O censo em `docs/type-annotation-measurement.md` ja mostrou que 78% dos
  sitios anotaveis quentes ja sao monomorficos, o que sugere que o perfil se
  reconstroi rapido depois de cada reset. Se `vec3_flat` e `poly_deopt` alcancarem
  o criterio de aceitacao com o perfil sendo zerado a cada scavenge, R4 vira
  otimizacao e nao requisito, e o indice de classe passa a se justificar so pelo
  `cmp` de 32 bits.
- **R5 e R6** caem se o formato no cabecalho nao couber junto com o indice de
  classe sem crescer o cabecalho alem de 16 bytes. Nesse caso o objeto fica mais
  caro por instancia e a conta muda.
