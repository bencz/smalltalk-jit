# ADR 0005: cabecalho de objeto de 8 bytes e forma do coletor

Data: 2026-07-27. Status: aceito. Fecha o que o ADR 0003 deixou em aberto.

## Decisao

**Cabecalho de 8 bytes**, uma palavra, no lugar dos 16 de hoje:

```
bits  0..21   classIndex      22 bits, indice numa tabela de classes
bits 22..43   identityHash    22 bits
bits 44..48   format           5 bits, ver abaixo
bits 49..56   slotCount        8 bits, 255 = "grande", tamanho numa palavra extra
bits 57..63   gc               7 bits: marked, forwarded, remembered, finalized,
                               pinned, e dois livres
```

O `format` responde SOZINHO, sem tocar a classe:

```
0  fixo, so ponteiros                 4  bytes indexado (String, ByteArray)
1  fixo, sem ponteiro nenhum          5  doubles crus indexado (FloatArray,
2  indexado por ponteiro (Array)         array plano de classe de valor)
3  fixo + indexado por ponteiro       6  forwarded
                                      7  free space
```

**Coletor**: nursery copiadora com TLAB por mutator (bump inline no codigo do
JIT), old space **mark-sweep NAO movente** com free lists segregadas por
tamanho. Escrita geracional por write barrier em store de ponteiro, remembered
set por thread consolidado no heap.

## Alternativas consideradas

**Cabecalho.** (a) manter 16 bytes e so trocar o ponteiro de classe por indice;
(b) 8 bytes estilo Spur, escolhida; (c) 16 bytes com formato e tamanho, sem
indice de classe.

**Old space.** (a) mark-sweep nao movente, escolhida; (b) mark-compact;
(c) mark-region estilo Immix com evacuacao oportunista.

## Criterio que decidiu

**O cabecalho de 8 bytes paga em quatro lugares de uma vez**, e cada um deles e
um requisito ja escrito do ADR 0003:

- `classIndex` de 22 bits faz o guard de classe ler **so o cabecalho**, sem
  derreferenciar o objeto classe e sem tocar uma segunda linha de cache: hoje
  sao um load da classe e um `cmp` de 64 bits. E resolve R4 de graca, porque um
  indice nao morre quando o objeto classe move, entao **as celulas de IC deixam
  de precisar do reset a cada scavenge** e o perfil passa a ser cumulativo.
  Esses dois eram a mesma decisao e agora sao a mesma linha.
- `format` resolve R5 e R6 juntos: o coletor sabe se o objeto tem ponteiro e
  sabe o tamanho sem as duas cargas dependentes (objeto -> classe -> shape) que
  paga hoje em cada objeto varrido.
- indice em vez de ponteiro **elimina a maior parte de `pointersOffsets`**: uma
  classe bakeada no codigo vira um imediato de 22 bits que nunca precisa ser
  atualizado por coleta nenhuma.
- 8 bytes a menos por objeto. Num `Vec3` de tres campos e 40 bytes hoje, cai
  para 32: 20%. Nao e o motivo principal, e nao e desprezivel.

O custo e uma indirecao para chegar ao objeto classe (`classTable[idx]`), paga
quando o codigo precisa da CLASSE e nao da IDENTIDADE dela. O caminho quente
(guard de IC, checagem de tipo, decisao de layout) precisa so da identidade,
que e o proprio indice. Lookup de metodo e reflexao pagam um load a mais, e sao
frios ou ja cacheados.

**Old space nao movente** porque mover objetos velhos e o unico motivo restante
para manter a maquinaria de atualizar ponteiros bakeados em codigo executavel, e
o indice de classe acabou de tirar a maior parte dela. Nao movente tambem torna
`become:` (R8) e pinning triviais, e a exec space ja e nao movente por
construcao. Fragmentacao e o preco; free lists segregadas por tamanho sao a
resposta padrao e, se medirmos que nao bastam, Immix entra como substituicao
LOCAL, atras da mesma interface, sem tocar no modelo de objeto.

**Nursery copiadora** fica porque o caminho de alocacao e um bump com teste de
limite, que e o que R7 pede e o que o JIT inlina.

Por que nao Immix agora: ele resolve fragmentacao, que ainda nao foi medida
neste VM, e custa complexidade num momento em que o oraculo de corretude e
fraco (ADR 0002, corte seco). Trocar o coletor depois e local; trocar o modelo
de objeto depois nao e.

## Consequencias que ja aceito

- `Snapshot.c` muda com o modelo. Nao e migracao: `run_tests.sh` regera a imagem
  a partir de `packages/Core` a cada execucao, entao o snapshot e derivado.
- `slotCount` de 8 bits obriga uma palavra de tamanho extra para objetos com
  mais de 254 slots. Todo leitor de tamanho passa por uma funcao, nunca pelo
  campo direto.
- Tabela de classes: uma classe nova aloca um indice; 22 bits dao 4.194.304, o
  que e teto suficiente e tem que dar erro limpo se estourar, nunca silencio.
- O bit 3 do ENDERECO continua separando young de old (`Object.h:25-34`, R9),
  independente do cabecalho.

## CORRECAO: "um `cmp` de 32 bits" estava errado

A primeira versao deste ADR afirmava que o guard viraria **um** `cmp` de 32 bits
contra imediato. A implementacao desmentiu, e o auto-teste do JIT pegou como um
guard que falhava para a classe CERTA.

O motivo: os bits 0..21 da palavra baixa do cabecalho sao o indice de classe,
mas os bits 22..31 sao **hash de identidade**, nao zero. Um `cmp` de 32 bits
contra o indice puro so casa quando o hash calha de ser zero.

O guard real sao **tres** instrucoes: carga de 32 bits do cabecalho, `and` com a
mascara de 22 bits, `cmp` contra o imediato. Continua sem derreferenciar a
classe e sem tocar segunda linha de cache, que era o ganho de verdade contra o
`load` + `cmp` de 64 bits do VM antigo. A afirmacao de "uma instrucao" era
otimismo, nao medicao.

O que custaria voltar a uma instrucao: reordenar o cabecalho para que os bits
0..31 sejam indice de classe (22) mais formato (5) mais cinco bits sempre zero,
com o hash mudando para a metade alta. O `cmp` unico entao compararia classe E
formato de uma vez, e o formato e constante por classe, entao o compilador o
conhece. O preco e o hash de identidade cair de 22 para 17 bits. **Nao esta
feito**: e uma instrucao contra cinco bits de hash, e trocar isso sem medir
seria repetir o erro que esta correcao registra.

## O que invalidaria esta decisao

- Se a indirecao `classTable[idx]` aparecer no perfil do lookup de metodo acima
  de uns 2% do total. A resposta seria cachear o ponteiro de classe ao lado do
  indice nos caminhos quentes, nao voltar ao ponteiro no cabecalho.
- Se 22 bits de hash de identidade produzirem colisao mensuravel em
  `IdentityDictionary` sob carga real. Hoje o hash tem 32.
- Se a fragmentacao do old space nao movente for medida e passar de uns 30% de
  desperdicio sob DeltaBlue ou sob o servidor HTTP. Ai entra Immix, atras da
  mesma interface.
