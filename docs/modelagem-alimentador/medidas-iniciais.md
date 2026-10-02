# Medidas iniciais

Ponto de partida, não verdade. As dimensões de peças compradas variam de
fabricante para fabricante: **confira cada uma com paquímetro na peça real**
antes de fechar o encaixe. O que está marcado *medir* não tem valor confiável
aqui — o Arthur tem todas as peças em mãos.

---

## 1. Parâmetros do modelo

Os nomes abaixo são os que o modelo paramétrico deve usar. Os que dependem do
aquário vêm da [`ficha-de-medicao.md`](./ficha-de-medicao.md); os valores da
coluna "exemplo" servem só para a conta da §2.

| Parâmetro | O que é | Valor | Exemplo |
|---|---|---|---|
| `H_borda` | Altura da borda superior do vidro acima da mesa | da ficha | 300 mm |
| `h_tampa` | Quanto a tampa ou a moldura sobe acima da borda do vidro (0 sem tampa) | da ficha | 0 |
| `e_vidro` | Espessura do vidro | da ficha | 6 mm |
| `h_agua_max` | Da borda até a água, com o aquário recém-completado | da ficha | 25 mm |
| `h_agua_min` | Da borda até a água, no fim da evaporação, antes de completar | da ficha | 45 mm |
| `D_torre` | Do eixo da torre à face externa do vidro | escolha | 55–70 mm |
| `F_min` | Folga mínima de qualquer ponto do módulo ao vidro e à tampa | fixo | **10 mm** |
| `A_bico` | Quanto a saída da calha avança além da face interna do vidro | escolha | 35 mm |
| `H_queda` | Altura da saída da calha acima da água no nível máximo | 25–40 mm | 30 mm |
| `theta_calha` | Inclinação da calha em relação à horizontal | **≥ 45°**; 50° recomendado | 50° |
| `x_tubo` | Do eixo da torre ao eixo do tubo vertical (o bloco do sensor), na direção do aquário | escolha | 25 mm |
| `h_sensor` | Do piso do slide ao começo do trecho inclinado da calha (bloco do sensor + curva) | ~35 mm | 35 mm |
| `h_reserv` | Do piso do slide ao topo da tampa do reservatório | ~60 mm | 60 mm |
| `h_base` | Altura da base | ~55 mm | 55 mm |
| `p_bolso` | Diâmetro do bolso do slide | 1,2–1,6 mm (jogo de calibração) | 1,4 mm |
| `S_curso` | Distância entre a saída do reservatório e a boca do tubo | ≥ 8 mm (`doseador.md`) | 8 mm |

---

## 2. A conta das alturas

A calha desce em linha reta, com inclinação `theta_calha`, do pé do bloco do
sensor até a saída sobre a água. Daí saem todas as alturas (medidas a partir
da mesa):

```text
alcance horizontal da calha  R       = D_torre − x_tubo + e_vidro + A_bico
saída da calha               z_saida = H_borda − h_agua_max + H_queda
começo do trecho inclinado   z_calha = z_saida + R · tan(theta_calha)
piso do slide                z_slide = z_calha + h_sensor
topo do módulo               z_topo  = z_slide + h_reserv
folga sobre a borda          z_saida + A_bico · tan(theta_calha)
                             − raio externo da calha − (H_borda + h_tampa)   ≥ F_min
                             (medida sobre a face interna do vidro, onde a
                             calha passa mais baixo)
```

Com os valores de exemplo (`D_torre` = 70 mm):

| Grandeza | Valor |
|---|---|
| `R` | 70 − 25 + 6 + 35 = **86 mm** |
| `z_saida` | 300 − 25 + 30 = **305 mm** |
| `z_calha` | 305 + 86 × tan 50° = 305 + 102 = **407 mm** |
| `z_slide` | 407 + 35 = **442 mm** |
| `z_topo` | 442 + 60 = **502 mm** |
| folga sobre a borda (calha com 6 mm de raio externo) | 305 + 35 × 1,19 − 6 − 300 = **41 mm** ✓ |
| altura da torre (do topo da base ao pé do topo) | ≈ `z_calha` − 10 − `h_base` = **342 mm** |

Para um módulo mais baixo: aproxime a torre do vidro (`D_torre` menor) ou leve
o tubo vertical mais para perto do aquário (`x_tubo` maior) — o que encurta
`R`. Não baixe `theta_calha` de 45°: abaixo disso, a ração úmida para no meio.

A haste do M2, na ponta da calha, desce até a água: comprimento ≈ `H_queda` + 5
mm de imersão no nível máximo, com **ajuste de altura de pelo menos**
`h_agua_min` − `h_agua_max` + 5 mm (no exemplo, ±15 mm), para continuar
tocando a água no fim da evaporação.

---

## 3. Peças compradas

### Base

| Peça | Envelope (mm) | Fixação | Observação |
|---|---|---|---|
| ESP32-C3 Super Mini | placa 22,5 × 18; ~3,5 com componentes; +8,5 na barra de pinos fêmea | na placa perfurada | Rasgo de ~12 × 6 mm para alcançar o USB-C com o cabo (recuperação) |
| Placa perfurada | 50 × 70 × 1,6 (pode ser cortada) | 4 parafusos M3 com espaçadores de 5 mm | Furos dos cantos: *medir* |
| DS3231 (módulo ZS-042) | ~38 × 22 × 14, com o suporte da CR2032 atrás | 2 parafusos M2,5 (*medir* os furos) | A bateria precisa sair sem desmontar o resto |
| Display OLED SSD1306 0,96" I²C | placa ~27,3 × 27,8 × 3,7; área ativa ~21,7 × 10,9 | 4 parafusos M2 (*medir* a distância entre furos) | No painel da frente, inclinado 10–20° para cima; janela recortada na área ativa |
| Botão de painel | *medir* (furo típico de 12 ou 16 mm) | porca do próprio botão | Ao lado do display |
| Buck LM2596 | ~43 × 21 × 14 | 2 parafusos M3 (furos em diagonal) | Esquenta pouco; perto de um furo de ventilação |
| Jack P4 de painel | *medir* (furo típico de 8 ou 11 mm) | porca | Na traseira |
| Cabo do controlador principal (4 vias, JST-XH) | conector ~12,4 × 5,8 × 7 | prensa-cabo ou abraçadeira ancorada por dentro | Na traseira; o puxão no cabo nunca chega à solda |
| Lastro | ≥ 300 g (arruelas de aço, chumbada, areia em saquinho) | bolsão no fundo, do lado oposto ao aquário | Ver §6 |

### Topo

| Peça | Envelope (mm) | Fixação | Observação |
|---|---|---|---|
| Servo MG90S | corpo ~22,8 × 12,2 × 28,5; abas com ~32,5 de comprimento total, furos Ø ~2,2 a ~27,8 entre centros | 2 parafusos nas abas, com arruela de borracha | *medir* — varia entre fabricantes; prever o cabo de 3 vias saindo para a plaquinha do topo |
| Motor de vibração 1027 (M1) | Ø 10 × 2,7 | adesivo do próprio motor | Área plana de 12 × 12 mm na **parede externa** do funil, perto da saída. Nunca dentro do reservatório |
| Motor 130 com peso excêntrico (M2, "pêndulo") | *medir* — o 130 comum tem corpo 27,5 × 20 × 15 e eixo Ø 2 × 8 | suporte na ponta da calha, com junta de TPU | O peso gira: prever o espaço livre dele |
| Módulo comparador LM393 | ~32 × 14 × 8 (*medir*) | 1 parafuso M3 ou encaixe | O trimpot precisa ficar acessível: é ajustado uma vez, na bancada |
| LED IR e fototransistor de 5 mm | Ø 5,0 (aba Ø 5,8) × 8,6 | furo Ø 5,1 com encosto na aba | Frente a frente no bloco do sensor (`doseador.md`) |
| Capacitor 470 µF/16 V | ~Ø 8 × 12 (*medir*) | na plaquinha do topo | Junto do conector do servo |
| Plaquinha de conectores do topo | ~30 × 40 | 2 parafusos M2 | Onde o chicote da torre se encaixa |
| Tubo de PTFE (o de impressora 3D) | DI 4 × DE 6 mm | preso na calha | O forro da calha: liso, não gruda, troca barato |

### Torre

Só o chicote: **doze fios** (2 de 24 AWG para o servo, o resto de 26 AWG) e,
na ponta de baixo, os conectores JST-XH — o maior de 4 vias, ~12,4 × 5,8 mm.
Canal interno de pelo menos **20 × 14 mm** (ou Ø 22 mm), para o conector
passar com os fios. Uma tampa lateral removível, ou a torre em duas metades
aparafusadas, facilita passar o chicote.

---

## 4. Impressão e tolerâncias

| Encaixe | Folga ou medida |
|---|---|
| Slide no canal (o encaixe crítico) | 0,2 mm na espessura e 0,3 mm na largura, no total |
| Furo passante para M3 | Ø 3,3–3,4 mm |
| Inserto de latão M3 / M2 (aquecido) | o furo da tabela do fabricante do inserto (tipicamente Ø 4,0–4,2 / Ø 3,2) |
| LED e fototransistor | furo Ø 5,1 mm |
| Tubo de PTFE de 6 mm | furo Ø 6,1–6,2 mm, com um degrau de encosto |
| Parede estrutural | ≥ 1,6 mm (4 perímetros de 0,4) |
| Parede do reservatório | ≥ 1,2 mm |

- Bico de 0,4 mm, camada de 0,2 mm; **0,12 mm** no slide, no canal e no bloco
  do sensor.
- Superfície que a ração toca: lisa, com as camadas no sentido em que o grão
  escorrega. O tubo vertical, impresso em pé.
- Balanços de até 45° sem suporte; suporte só onde não toca ração.
- Peças que se desmontam: **insertos de latão**, nunca parafuso direto no
  plástico.

## 5. Materiais

| Material | Onde | Por quê |
|---|---|---|
| **PETG** natural (sem corante) | Reservatório, slide, canal, calha | Aguenta a umidade e o calor da luminária sem deformar (o PLA flui com o tempo); natural, dá para ver o nível da ração |
| **PETG** de qualquer cor | Base, torre, estrutura do topo | — |
| **Material opaco ao infravermelho** | Bloco do sensor | Alguns plásticos pretos são transparentes a 940 nm: o feixe vazaria pela parede. Confira com o `teste sensor` do firmware; se vazar, fita de alumínio por fora ou tinta |
| **TPU 95A** | Junta do M2, arruelas do servo, pés (se não for EVA) | Isola vibração |
| POM (Delrin), opcional | Slide | Desliza melhor e gasta menos que PETG; chapa de 1,5 mm cortada a laser ou fresada |

## 6. Massa e estabilidade

| Parte | Massa estimada |
|---|---|
| Topo + calha + reservatório cheio | 130–160 g |
| Torre | 40–60 g |
| Base com a eletrônica, sem lastro | 150–200 g |

O topo fica em balanço na direção do aquário. A torre deve sair **perto da
borda da base do lado do aquário** (a uns 25 mm dela), com a base se estendendo
para o outro lado, onde vai o lastro: assim o peso da base trabalha contra o
tombamento. Base com pelo menos **150 × 120 mm** de pegada e **300 g de
lastro**.

Com o módulo alto (topo a mais de 40 cm da mesa), um empurrão no topo ao
reabastecer derruba qualquer base razoável: além do EVA colado sob a base, cole
o EVA na mesa com fita dupla-face de espuma, ou use a fixação lateral.

## 7. Fixação

- **Sobre a mesa:** EVA macio (3–5 mm) colado sob a base inteira.
- **Na lateral da mesa:** um suporte em L preso à lateral da mesa, com 4
  coxins de borracha (*silentblock* M3 ou M4) entre ele e a base. A base deve
  ter, por baixo, os 4 insertos para esse suporte, mesmo na versão de EVA.
- **Base ↔ torre ↔ topo:** encaixe com chaveta (só monta de um jeito) e 2
  parafusos M3 em insertos. No topo, um encaixe redondo que permita **girar
  ±30°** antes de apertar, para mirar a calha no ponto onde o peixe come.
