# BettaCare como auxílio de IoT para aquarismo

**Para a reunião com o orientador — IFPE, Tecnólogo em ADS**

Reformular o enquadramento do TCC — de "meu controlador de aquário" para um
sistema de referência em automação e confiabilidade de dados aplicável a
aquarismo em geral — sem reabrir nenhuma decisão já tomada sobre o projeto em
si.

- Sistema em produção
- ESP32 + Node/Hono + PostgreSQL + PWA
- Nenhuma dívida de desenvolvimento pendente

---

## 1. O problema e o objetivo

### Problema

Quem cuida de um aquário em casa monitora temperatura, luz e ventilação
manualmente ou com soluções pontuais — um relé e um app comercial — que não
guardam histórico, não distinguem "sistema fora do ar" de "sensor com
defeito", e tratam todo peixe como se tivesse a mesma faixa ideal de
temperatura.

Na literatura de automação de aquarismo, a maioria dos trabalhos é prova de
conceito de um sensor e um atuador — sem tratar confiabilidade de dado como
problema.

### Objetivo

- Consolidar o BettaCare como estudo de caso de automação e observabilidade
  IoT aplicável a aquarismo, não só ao aquário do autor
- Tratar particionamento, idempotência de comando e diagnóstico de causa raiz
  como contribuição central — não a generalização em si
- Generalizar o mínimo necessário: múltiplos tanques e presets por espécie,
  ambos já latentes na arquitetura atual
- Deixar base de conhecimento e recomendações abertas como trabalho futuro,
  não como entregável

---

## 2. O que já existe vs. o que seria acrescentado

| Frente | Hoje (BettaCare em produção) | Generalização proposta |
|---|---|---|
| **Modelo de dados** | 🟢 **Pronto** — toda tabela (dispositivo, estado, telemetria, comandos) já é chaveada por `device_id`; a API já aceita `?device=` | 🟡 **A acrescentar** — só falta a interface: um seletor de tanque na tela, não mudança de schema |
| **Configuração por espécie** | 🟢 **Pronto** — o schema de configuração já guarda horário de luz e limiares de temperatura da ventoinha por dispositivo | 🟡 **A acrescentar** — uma tabela de presets (espécie → esses mesmos valores) que pré-preenche o formulário existente |
| **Avaliação de saúde** | 🟢 **Pronto** — motor de regras puro (`health/evaluate.ts`) já traduz telemetria em veredito por componente | 🟡 **A acrescentar** — reenquadrar como motor de recomendação: as faixas passam a vir do preset de espécie, não de constante fixa |
| **Confiabilidade & observabilidade** | 🔵 **Diferencial** — particionamento mensal, idempotência por sequência, distinção servidor-fora-do-ar × controlador-sem-contato, watchdog | Este é o argumento acadêmico central — ver seção 3 |
| **Base de conhecimento de aquarismo** | ⚪ Não existe hoje | Fica fora do escopo — ver seção 5 |

---

## 3. O argumento que sustenta o enquadramento

> **Achado da pesquisa de trabalhos relacionados**
>
> "O espaço de IoT aplicado a aquário é dominado por protótipos de sensor
> único e relé; o BettaCare já está estruturalmente acima da mediana dessa
> literatura sem generalizar nada — o argumento vencedor não é 'atende vários
> aquários', é ter tratado confiabilidade de dado como problema de primeira
> classe."

Há uma faixa mais séria de trabalhos centrada em arquitetura e
edge-computing para monitoramento aquático, e a produção formal (ACM) já
trata "gestão de aquário" como categoria própria de sistema — não é um
enquadramento inventado, é um espaço com publicação ativa. O BettaCare entra
nessa faixa pela engenharia que já foi feita: dado particionado, idempotência
de comando, causa raiz distinguida entre elétrica e software.

**Precedente direto encontrado:** um TCC de tecnólogo no IFPB — ESP32,
sensor de temperatura, atuador, frontend web, testado em aquário único — foi
aceito como estudo de caso sem nenhuma generalização (ref. 1). Isso muda a
estratégia: generalizar **não é pré-requisito** para o TCC ser aceito nesse
formato de curso — é uma escolha de diferenciação, com custo controlado
porque a arquitetura já sustenta os dois eixos propostos.

---

## 4. Referências levantadas

1. **Sistema IoT para automação e monitoramento inteligente para aquários**
   — TCC, Tecnólogo, IFPB.
   <https://repositorio.ifpb.edu.br/handle/177683/4386>

2. **FishTank: an IoT-based Smart Aquarium Management System for Freshwater
   Fish Enthusiasts** — ACM ICIBE 2023.
   <https://dl.acm.org/doi/10.1145/3629378.3629450>

3. **A Modularized IoT Monitoring System with Edge-Computing for
   Aquaponics**.
   <https://pmc.ncbi.nlm.nih.gov/articles/PMC9739085/>

4. **Sobre a validade metodológica do estudo de caso único** — Revista
   Produção, SciELO.
   <https://www.scielo.br/j/prod/a/zhVnv4mW8pvWc3hTxvfXt4L/>

---

## 5. Onde parar

### Prometer ao orientador

- Rótulo de "múltiplos tanques" na interface — o dado já suporta
- Tabela de presets por espécie alimentando a configuração existente
- Reenquadrar o motor de saúde como recomendação baseada em regra
- Estudo de caso único como formato de fallback, já com precedente aceito

### Deixar fora, ou virar "trabalho futuro"

- Base de conhecimento completa de aquarismo — apps maduros (SpawnOS,
  Aquarimate) já cobrem isso
- Suporte a espécies fora de peixes de água doce de pequeno porte
- Qualquer coisa que exija hardware que o aquário do autor não tem

---

## 6. Maquete: o que muda na prática

Duas telas do PWA hoje, ao lado de como ficariam com a generalização mínima
da seção 2 — mesmos componentes visuais do app, nada reinventado.

### Cabeçalho — de um aquário fixo para um seletor de tanque

| Hoje | Proposto |
|---|---|
| Cabeçalho fixo: **"BettaCare"** | Cabeçalho vira seletor: **"Aquário do Arthur ▾"** |

O nome fixo vira um seletor. Um usuário com dois tanques troca entre eles;
com um só, o seletor nem aparece.

### Configuração — de limiares soltos a um preset de espécie

| Hoje | Proposto |
|---|---|
| Cartão "Ventoinha — limiares" direto, sem contexto de espécie:<br>`Liga em 29.0°C` · `Desliga em 27.5°C` | Acima do mesmo cartão, um seletor de preset — **"Betta (Betta splendens)"** — que preenche os mesmos campos e acrescenta uma dica:<br>`Liga em 29.0°C` · `Desliga em 27.5°C`<br>*"Faixa recomendada: 24–28°C. Preenchido pelo preset — ajustável à mão como hoje."* |

O preset só preenche os mesmos campos que o formulário atual já tem. O aviso
reaproveita o componente de dica que a aba Saúde já usa.

---

## Fechamento

Nada aqui exige recomeçar código ou pausar a rodada de confiabilidade em
andamento. A pauta da reunião é decidir entre o caminho de estudo de caso
único (mais rápido, com precedente aceito) e o pacote mínimo de
generalização acima (mais argumento de banca, custo controlado).

*BettaCare · IFPE · Tecnólogo em ADS*
