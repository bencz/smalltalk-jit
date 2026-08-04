# stjit — protótipo de validação de um JIT para Smalltalk

Protótipo executável construído para testar uma afirmação concreta:

> um laço de produto escalar sobre um `Array of: Vec3` deve compilar para
> código sem uma única alocação e sem um único acesso *tagged* dentro do laço.

Não há geração de código de máquina. O tier 2 compila para closures Python
tipadas sobre um arquivo de registradores plano. O que valida a afirmação não
é o relógio, é a **instrumentação**: cada alocação, cada box, cada unbox e cada
leitura tagged incrementa um contador em `stjit/value.py`, e o laço otimizado
não toca nenhum deles.

## Como rodar

```
python3 validate.py
```

Sem dependências. Python 3.8 ou superior.

## Resultado medido

Laço interno de `Main>>hotFlat:` após otimização, 13 operações, todas cruas:

```
guard_class {ValueArray(Vec3)} (v42)
vaload {0} (v42, v28) : f64      vaload {1} ...      vaload {2} ...
fmul  fmul  fmul                 (o scale:)
fmul  fmul  fadd  fmul  fadd     (o dot:)
fadd  (acumulador f64)
iadd  (contador i64)
```

200 elementos, 50 iterações, ou seja 10.000 iterações internas:

| contador       |   tier 0 |  tier 2 |
|----------------|---------:|--------:|
| allocs         |   20.000 |       0 |
| unboxes        |  210.000 |       0 |
| tagged_loads   |  110.050 |       2 |
| tagged_ops     |  200.201 |       2 |
| sends          |  170.202 |       1 |
| guard_checks   |        0 |  10.101 |
| tempo          |   0,50 s |  0,018 s |

Speedup de cerca de 28x, com resultado numérico idêntico (149876875.0,
conferido independentemente). Tempo de compilação de 1 a 3 ms.

O array plano contra um `Array` genérico de ponteiros para `Vec3`, com a
mesma semântica e ambos já otimizados até zero alocações:

| contador     |   plano | ponteiros |
|--------------|--------:|----------:|
| tagged_loads |       2 |    20.002 |
| guard_checks |  10.101 |    20.101 |

Isso isola o custo do layout depois que todas as outras otimizações já
aconteceram.

## Arquitetura

```
front.py    lexer + parser (sintaxe GNU + anotações de tipo)
compile.py  AST -> bytecode de registradores, três endereços
            blocos de controle inlinados estruturalmente
runtime.py  tier 0: interpretador com caches inline
value.py    tagging de 64 bits + heap + contadores
lower.py    inlining guiado por perfil + construção de SSA (Braun et al.)
ir.py       IR em SSA, representação como parte do valor
passes.py   otimizador
backend.py  tier 2: SSA -> closures tipadas + desotimização
driver.py   política de tiering e recompilação
```

### Representação de valores

Tagging de 64 bits com `bit0 == 0` para SmallInteger, de modo que o guard de
aritmética inteira seja um `or` seguido de `test`. Ponteiros, caracteres,
imediatos e SmallFloat compartilham `bit0 == 1` discriminados pelos bits 2..1.
SmallFloat usa 1 bit de sinal, 8 de expoente e 52 de mantissa, com uma janela
de expoente, no estilo do Spur: floats fora da janela vão para o heap e isso é
contado.

O índice de classe fica no cabeçalho do objeto, não um ponteiro, para que o
guard de um cache inline seja um `cmp` de 32 bits.

### Classes de valor

`Vec3 := Value [ | x::Float64 y::Float64 z::Float64 | ]` declara uma classe
imutável cujos campos são doubles crus embutidos no objeto. `Vec3 arrayNew: n`
aloca um array com layout plano: `n * 3` doubles contíguos, sem cabeçalho e
sem ponteiro por elemento.

Cada `ValueArray of: C` é uma classe própria, com índice próprio. Sem isso o
cache inline diria apenas "é um ValueArray" e o otimizador não saberia o layout
do elemento.

### Feedback de tipos

Aritmética, `at:`, `at:put:` e `size` **não** são compilados como primitivas.
Vão como SEND normal, para que o cache inline colete perfil. O otimizador
aprende com o feedback, não com o compilador trapaceando. Isso é o ponto do
Strongtalk: mesmo com tipos declarados disponíveis, o perfil do PIC é mais
preciso na prática para decidir despacho e inlining. Os tipos aqui servem para
**layout e representação**, não para dispatch.

### Ordem dos passes

A ordem importa mais que os passes individuais:

1. phis triviais, para limpar o ruído da construção de SSA
2. propagação de tipos
3. guards redundantes
4. substituição escalar
5. GVN, que é onde `box(unbox(x))` desaparece
6. LICM
7. promoção de representação dos phis carregados pelo laço
8. DCE
9. fusão de blocos

O passo 7 é o que a maioria dos protótipos esquece. Um acumulador
`total := total + x` nasce como phi TAGGED cujo único produtor é um `box_f` e
cujo único consumidor é um `unbox_f`. Sem promover o phi para F64, toda a
eliminação de boxing para na fronteira do laço.

### Desotimização

Cada guard carrega um estado de desotimização: uma pilha de frames virtuais do
interpretador, cada um com o método, o bci e o mapeamento de registradores para
valores SSA. Os registradores incluídos são só os vivos naquele bci, calculados
por uma análise de vivacidade retroativa sobre o bytecode original.

O frame mais interno reexecuta a instrução que falhou; os externos já tinham a
chamada em voo, então retomam na instrução seguinte ao send com o resultado
depositado no registrador de destino.

Quando a análise de escape apaga um objeto, ela deixa no lugar uma **receita de
materialização**: classe mais a lista de valores SSA que preenchem os campos.
É isso que torna a eliminação segura. O teste 4 da bateria força exatamente
esse caso: um objeto eliminado que ainda está vivo quando um guard falha, e o
resultado tem que bater até a última unidade.

### Convergência

Depois de três desotimizações, o método é recompilado com o perfil atualizado e
marcado como "especulação queimada": a partir daí só volta a especular em
sítios estritamente monomórficos. Na prática o send vira residual e o sistema
para de oscilar entre otimizar e desotimizar.

## O que a bateria verifica

27 verificações, agrupadas em quatro testes:

1. pipeline completo: análise estática do laço interno, comparação de
   contadores entre tier 0 e tier 2, igualdade de resultado
2. efeito do layout: plano contra ponteiros
3. desotimização: guard falhando no meio do laço, resultado correto,
   convergência por recompilação
4. materialização de objeto eliminado, com resultado exato

## Limitações, sem maquiagem

Isto é um protótipo de validação, não um sistema. O que falta ou está
simplificado:

- **Sem código de máquina.** O tier 2 são closures Python tipadas. A alocação
  de registradores é trivial, um slot por valor SSA, sem reuso. Um sistema real
  usaria linear scan com splitting.
- **Sem closures de verdade.** Só blocos literais em `ifTrue:`, `ifFalse:`,
  `whileTrue:`, `to:do:`, `timesRepeat:`, `and:` e `or:` são aceitos, e são
  inlinados estruturalmente. Não há `BlockClosure`, `value`, `value:`, nem
  retorno não local atravessando frames separados. Como consequência, o ponto
  sobre retorno não local virar um branch é demonstrado apenas no caso fácil,
  em que o método home está no mesmo frame.
- **Guard invariante do laço não é hoisted.** O `guard_class` sobre o array
  fica dentro do laço, um `cmp` por iteração (os 10.101 acima). Hoistar exigiria
  reescrever o estado de desotimização para o preheader ou versionar o laço.
  Preferi deixar honesto a fingir.
- **`known` no construtor de SSA é insensível a fluxo.** Correto na prática
  porque os guards sempre precedem os usos no código gerado, mas não é sound em
  geral.
- **Sem GC.** O heap só cresce. Sem `become:`, sem invalidação por redefinição
  de classe. A análise de hierarquia existe (`unique_implementor`) mas não é
  usada para remover guards, justamente porque não há infraestrutura de
  invalidação por dependências.
- **Sem PIC de verdade no tier 2.** Um sítio que fica bimórfico vira send
  residual, em vez de inlining polimórfico com switch de tipo.
- Divisão inteira `/` produz float em vez de Fraction. Overflow de SmallInteger
  levanta erro em vez de promover para LargeInteger.

## Próximo passo natural

O que eu construiria em seguida, nesta ordem: closures reais com mapeamento
contexto-para-pilha, inlining polimórfico de duas vias com switch de tipo, e só
então backend nativo. A infraestrutura de desotimização, que costuma ser o
ponto onde esses projetos travam, já está de pé e testada.
