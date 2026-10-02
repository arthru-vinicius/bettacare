# Ficha de medição

Para o Arthur preencher **no aquário**, antes de o modelador fechar as alturas
da torre e da calha. Cada linha vira um parâmetro de
[`medidas-iniciais.md`](./medidas-iniciais.md). A coluna "exemplo" mostra o
formato esperado, com números inventados — não são do aquário real.

Trena para o que for grande, paquímetro para o resto. Tire as fotos da última
seção com uma régua aparecendo: elas tiram dúvidas que a tabela não tira.

---

## O aquário

| # | Medida | Parâmetro | Exemplo | Medido |
|---|---|---|---|---|
| 1 | Largura × profundidade × altura, por fora | — | 400 × 250 × 300 mm | |
| 2 | Altura da borda superior do vidro acima da mesa | `H_borda` | 300 mm | |
| 3 | Espessura do vidro | `e_vidro` | 6 mm | |
| 4 | A borda tem moldura? Quanto ela sobe acima do vidro e quanto avança para dentro? | `h_tampa` | sem moldura | |
| 5 | Tem tampa? De quê? Ela sobe quanto acima da borda? | `h_tampa` | vidro, apoiada 5 mm abaixo da borda | |
| 6 | A tampa tem abertura para alimentar? Posição e tamanho | — | recorte de 60 × 40 mm no canto traseiro esquerdo | |
| 7 | Da borda até a água, com o aquário **recém-completado** | `h_agua_max` | 25 mm | |
| 8 | Da borda até a água, **no fim da evaporação**, antes de completar | `h_agua_min` | 45 mm | |
| 9 | De quanto em quanto tempo completa a água | — | semanal | |
| 10 | Onde fica a luminária, e se a luz bate onde ficaria a saída da calha | — | barra de LED no centro da tampa | |
| 11 | Onde fica o filtro, e para onde ele empurra a água na superfície | — | canto direito, empurra para a esquerda | |
| 12 | Onde o peixe costuma comer (o ponto que a calha deve mirar) | `A_bico` | frente, a ~4 cm do vidro lateral | |

O filtro importa: uma correnteza forte na superfície leva o grão embora antes
de o peixe pegar, e pode afastar a água da haste do M2.

## A mesa e o espaço ao lado

| # | Medida | Parâmetro | Exemplo | Medido |
|---|---|---|---|---|
| 13 | Espaço livre na mesa ao lado do aquário (largura × profundidade) | pegada da base | 200 × 250 mm | |
| 14 | Desse espaço, de que lado o módulo ficaria: lateral do aquário ou frente? | — | lateral esquerda | |
| 15 | Distância do aquário até a beira da mesa, nesse lado | — | 120 mm | |
| 16 | Espessura do tampo da mesa e material (para a fixação lateral) | — | 18 mm, MDF | |
| 17 | Prefere EVA sobre a mesa ou fixação na lateral? | — | EVA | |
| 18 | Altura livre acima do aquário (prateleira, armário) | limite de `z_topo` | 450 mm acima da borda | |

## Fios e energia

| # | Medida | Exemplo | Medido |
|---|---|---|---|
| 19 | Onde fica o controlador principal, e o comprimento do cabo de 4 vias até o lugar do módulo | atrás do aquário, 80 cm | |
| 20 | Tomada para a fonte própria do módulo (se ele for funcionar sem o cabo) | régua atrás do móvel | |

## A ração

| # | Medida | Exemplo | Medido |
|---|---|---|---|
| 21 | Marca e produto | — | |
| 22 | Diâmetro de 10 grãos com paquímetro: menor, médio e maior | 0,9 / 1,0 / 1,2 mm | |
| 23 | Formato: esfera, cilindro, irregular | esfera | |
| 24 | A ração gruda em plástico quando o ar está úmido? (deixe uns grãos num pote destampado ao lado do aquário por um dia) | não | |

O bolso do slide e o feixe são dimensionados pelo **maior** grão da linha 22.

## Fotos (com uma régua visível)

1. Lateral do aquário, do lado onde o módulo vai ficar, mostrando a mesa.
2. A borda de cima, de perto: vidro, moldura e tampa.
3. A tampa vista de cima, com a abertura de alimentação.
4. O ponto onde o peixe come, visto de cima.
5. Dez grãos de ração sobre o papel milimetrado.
