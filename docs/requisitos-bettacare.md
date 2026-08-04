# Requisitos para o bettacare rodar neste homelab

Documento de contrato entre a aplicação **bettacare** e o servidor. Escrito para
quem vai reescrever a aplicação, e que não participou do provisionamento.

O servidor já está pronto e validado. Nada aqui pede mudança na infraestrutura:
são as condições que a aplicação precisa satisfazer para entrar sem exceções.

Onde este documento disser **DEVE**, é requisito rígido -- descumprir quebra
alguma garantia já estabelecida. Onde disser **INFORMAR**, é um dado que o
playbook precisa receber para gerar a configuração.

> **Nota**: este repositório é público. Endereços e nomes de host reais foram
> substituídos por marcadores como `<IP_DO_SERVIDOR>` e `<HOSTNAME_PUBLICO>`.
> Os valores concretos ficam fora do versionamento.

---

## 1. O ambiente, em uma página

| Item | Valor |
|---|---|
| Servidor | Debian 13, 2 núcleos / 4 threads, 8GB RAM, NVMe 256GB |
| Runtime | Docker com plugin compose |
| Rede interna | rede Docker `backend`, externa e compartilhada |
| Banco | PostgreSQL 17 em container, compartilhado, **sem porta publicada** |
| Entrada pública | Cloudflare Tunnel + Cloudflare Access |
| Entrada local | bind explícito em `<IP_DO_SERVIDOR>` |
| Hostname público | `<HOSTNAME_PUBLICO>` (Access já configurado) |
| Fuso do servidor | `America/Recife` |

A máquina é modesta e compartilhada com o Postgres, o cloudflared e outros
serviços do homelab. Consumo importa.

---

## 2. Empacotamento

- **DEVE** haver um `Dockerfile` no repositório da aplicação.
- **DEVE** ser construída em CI (GitHub Actions) e publicada em
  `ghcr.io/arthru-vinicius/<nome>:<tag>`.
- **DEVE** usar tag versionada (`v1.2.3` ou o SHA do commit). **Nunca `latest`**:
  sem versão não há como saber o que está rodando nem voltar atrás.
- **DEVE** ser `linux/amd64`.
- **NÃO** construir a imagem no servidor. São 2 núcleos competindo com tudo o
  que já roda; um build de frontend ali leva dezenas de minutos.
- Se o repositório for privado, **INFORMAR** -- o servidor precisará de um token
  de leitura do `ghcr.io`, que entra no vault.

### O usuário do processo dentro do container

**INFORMAR o uid com que o processo roda.** Este item já quebrou dois serviços
neste servidor e é a causa menos óbvia de falha em produção.

Arquivos com segredo são escritos no host com permissão restrita e montados no
container. Se o processo rodar como um uid que não os enxerga, ele falha ao
subir -- e a mensagem de erro raramente aponta para permissão. O playbook
precisa saber o uid para atribuir a posse corretamente.

Se a imagem for distroless ou rodar como `nonroot`, dizer explicitamente.

---

## 3. Rede: duas portas de entrada com regras opostas

Este é o ponto mais importante do documento. A aplicação tem **dois públicos com
exigências incompatíveis**, e eles não podem compartilhar hostname.

```
                  internet                          LAN <SUB-REDE_LOCAL>
                     |                                       |
            <HOSTNAME_PUBLICO>                            ESP32
                     |                                       |
           [ Cloudflare Access ]  <- exige navegador         |
                     |                                       |
              Cloudflare Tunnel                              |
                     |                                       |
        +------------+------------+                          |
        |     container bettacare  |<-------------------------+
        |  UI: porta interna       |   bind <IP_DO_SERVIDOR>:<porta>
        |  ingest: porta interna   |   token em header
        +--------------------------+
```

### 3.1 A interface web

- **DEVE** escutar em uma porta interna documentada. **INFORMAR qual.**
- **NÃO DEVE** publicar porta no host. O `cloudflared` alcança o container pelo
  nome dele dentro da rede `backend` -- publicar porta não só é desnecessário
  como contorna o firewall (ver seção 7).
- O nome do container será **`bettacare`**, porque vira o hostname no DNS
  interno do Docker. A regra de ingress apontará para
  `http://bettacare:<porta-interna>`.
- **DEVE** falar **HTTP puro**. O TLS termina na borda da Cloudflare; o
  `cloudflared` conversa em texto claro com a origem, dentro da máquina.
- **NÃO DEVE** redirecionar HTTP para HTTPS por conta própria. A requisição
  chega como HTTP e o redirect produziria laço infinito.
- **DEVE** confiar no cabeçalho `X-Forwarded-Proto` para montar URLs absolutas,
  senão links e redirects sairão como `http://` para o usuário final.
- **NÃO** incluir Caddy, Nginx ou qualquer proxy dentro do stack. O
  `cloudflared` já roteia. Um proxy a mais só adiciona um salto e uma
  configuração de TLS que vai tentar emitir certificado e falhar.

### 3.2 O endpoint do ESP32

- **DEVE** ser servido em uma porta interna **separada** da interface web, ou em
  um path que possa ser publicado isoladamente. **INFORMAR a escolha.**
- **DEVE** ser publicado com bind explícito de endereço:

  ```yaml
  ports:
    - "<IP_DO_SERVIDOR>:<porta>:<porta-interna>"
  ```

  Sem o prefixo de endereço, a porta é exposta em todas as interfaces e o
  firewall não tem como impedir.
- **NÃO DEVE** entrar nas regras de ingress do túnel. Um ESP32 fazendo POST
  atrás do Cloudflare Access receberia uma página HTML de login como resposta e
  falharia -- ele não sabe interpretar isso.

---

## 4. O ESP32

A arquitetura anterior usava um broker MQTT (HiveMQ Cloud) com mensagens
retidas. **Isso foi eliminado.** Não haverá broker: nem gerenciado, nem
Mosquitto local -- foi avaliado e descartado.

O ESP32 e o servidor estão na mesma rede física. A comunicação passa a ser
**HTTP direto na LAN**.

### 4.1 Consequência que não é óbvia

As mensagens *retidas* do MQTT funcionavam como o estado atual do sistema: quem
conectasse recebia o último valor de cada tópico. Sem broker, **esse estado
precisa ser persistido pela aplicação**. É a razão pela qual o bettacare passa a
precisar de banco de dados, coisa que a versão anterior não usava.

A aplicação **DEVE** manter, no PostgreSQL, o estado corrente do aquário --
última leitura de cada sensor, estado da luz, da bomba, do aquecedor -- além do
histórico, se desejado.

### 4.2 Contrato do endpoint de telemetria

- **DEVE** autenticar por **token em header**, mesmo sendo tráfego local. O
  cabeçalho já usado pelo firmware é `X-Api-Token`.
- **DEVE** comparar o token em tempo constante, para não vazar informação por
  diferença de tempo de resposta.
- **DEVE** responder rápido e com corpo pequeno. O ESP32 tem memória escassa.
- **DEVE** tolerar reenvio. Se a resposta se perder, o dispositivo vai repetir o
  POST -- o mesmo dado chegando duas vezes não pode duplicar registro nem
  corromper o estado.
- **DEVE** carimbar o horário de recebimento no servidor. O RTC do ESP32 deriva;
  se o dispositivo mandar o próprio timestamp, guarde os dois.
- **NÃO DEVE** exigir TLS do dispositivo. É tráfego de LAN e o custo de TLS num
  ESP32 não se paga aqui.
- **INFORMAR**: método, path, formato do corpo, resposta esperada e **com que
  frequência** o dispositivo envia. A frequência define o volume do banco.

### 4.3 O que já existe no firmware

Do levantamento anterior, permanece válido:

- O ESP32 expõe API HTTP local própria, autenticada com `X-Api-Token`
- Faz OTA por Basic Auth
- Guarda configuração (horários, limiares) em NVS, no próprio dispositivo

---

## 4.4 O sentido servidor → ESP32, e o problema de endereço

**Confirmado com o usuário:** o servidor vai precisar comandar o dispositivo --
alternar a luminária, controlar o aquecedor e afins. Ou seja, a comunicação é
nos dois sentidos, não apenas telemetria.

Isso traz uma limitação concreta:

> **O roteador é da operadora e o usuário não tem acesso administrativo.** Não é
> possível criar reserva de DHCP. Nada garante que o ESP32 mantenha o mesmo IP
> depois de uma queda de energia ou de o roteador reiniciar.

Foi exatamente por isso que o IP do servidor (`<IP_DO_SERVIDOR>`) precisou ser
validado como livre com `ping` e `arping` antes de ser fixado no
`/etc/network/interfaces`.

Abaixo as opções, da que eu recomendo para a que eu evitaria. **A decisão é do
agente do bettacare junto com o usuário** -- este documento só apresenta o
terreno.

### Opção A -- Inverter o sentido: o ESP32 pergunta se há comando

O dispositivo, que já fala com o servidor para mandar telemetria, passa também a
perguntar periodicamente se há comando pendente:

```
ESP32 --> POST /telemetria      (a cada N segundos)
ESP32 --> GET  /comandos        (a cada N segundos)  --> [] ou [{acao: "luz", estado: "on"}]
```

O servidor nunca inicia conexão. **O problema de endereço deixa de existir** --
não importa qual IP o ESP32 tem, porque ninguém precisa alcançá-lo.

- **A favor:** elimina o problema em vez de administrá-lo. É como a maioria dos
  dispositivos IoT comerciais funciona, justamente porque quase todos vivem
  atrás de NAT. Não exige IP fixo, nem porta aberta, nem descoberta.
- **Contra:** a latência é o intervalo de consulta. Com 3 segundos, apertar o
  botão da luz na interface e ela acender demora até 3 segundos.
- **Observação:** para luz, bomba e aquecedor, alguns segundos de latência são
  irrelevantes. Não é um controle que exige resposta instantânea.

**É a que eu recomendaria.** Resolve o problema de forma definitiva e o custo é
uma latência que ninguém vai perceber neste caso de uso.

### Opção B -- Conexão persistente iniciada pelo dispositivo

O ESP32 abre uma conexão WebSocket (ou SSE) com o servidor e a mantém aberta. O
servidor empurra comandos por ela quando quiser.

- **A favor:** latência praticamente zero, e continua sem precisar de endereço
  estável, porque quem conecta é o dispositivo.
- **Contra:** mais complexidade no firmware -- reconexão, keepalive, tratamento
  de queda. E uma conexão aberta consome memória do ESP32, que é escassa.

Vale se a latência da Opção A incomodar na prática.

### Opção C -- O ESP32 informa o próprio IP na telemetria

Mantém o servidor chamando o dispositivo, mas o dispositivo passa a incluir o
próprio endereço no corpo da telemetria. O servidor guarda esse valor e o usa
para as chamadas de saída.

```json
{ "ip": "<IP_DO_ESP32>", "temperatura": 26.4, "luz": "on" }
```

- **A favor:** se autocorrige. Mudou o IP, o próximo envio de telemetria
  atualiza o registro. Nenhuma configuração de rede envolvida.
- **Contra:** existe uma janela entre a troca de IP e o envio seguinte em que os
  comandos falham. O servidor precisa tratar essa falha com retentativa, e não
  supor que o endereço guardado ainda vale.

É o meio-termo razoável se a arquitetura de empurrar comandos for preferida.

### Opção D -- IP fixo no firmware

Configurar o ESP32 com endereço estático, fora da faixa que o roteador
distribui, seguindo o que já foi feito com o servidor.

- **A favor:** simples e previsível.
- **Contra:** grava a topologia da rede dentro do dispositivo. Trocar de
  roteador, mudar a faixa de endereços ou levar o aquário para outra casa exige
  regravar o firmware. E há risco de conflito: sem acesso ao roteador, não dá
  para reservar o endereço, apenas torcer para que ele não seja distribuído a
  outro aparelho.
- **Mitigação:** validar o endereço como livre com `ping` e `arping` antes de
  adotar, e escolher um número alto da faixa, longe de onde o DHCP costuma
  começar.

Funciona, mas troca um problema de software por um de configuração manual.

### Opção E -- mDNS

O ESP32 anuncia `bettacare.local` na rede, e o servidor o encontra pelo nome. O
`avahi-daemon` já está instalado e ativo no servidor, e o ESP32 tem suporte a
mDNS tanto no ESP-IDF quanto no Arduino.

**Não recomendo neste caso**, por um detalhe de implantação: a aplicação roda em
container, com rede em modo bridge. Multicast do container para a LAN física não
funciona de forma confiável, então a resolução mDNS falharia justamente de onde
ela precisa acontecer. Contornar isso exigiria resolver no host e repassar o
resultado, ou usar rede host no container -- o que está proibido por outra
razão (seção 8).

### Resumo

| Opção | Resolve o endereço? | Latência | Complexidade |
|---|---|---|---|
| A -- ESP32 consulta comandos | sim, elimina | intervalo de consulta | baixa |
| B -- conexão persistente | sim, elimina | mínima | média no firmware |
| C -- ESP32 informa o IP | sim, com janela de falha | mínima | baixa |
| D -- IP fixo no firmware | sim, com risco de conflito | mínima | baixa, mas manual |
| E -- mDNS | não funciona do container | -- | -- |

**INFORMAR qual caminho foi escolhido.** Se for A ou B, não há nada a fazer na
infraestrutura. Se for C ou D, também não -- mas o servidor precisará alcançar o
dispositivo, o que hoje já é possível: a saída não é filtrada e o container tem
rota para a LAN.

---

## 5. Banco de dados

Já provisionado e esperando:

| Item | Valor |
|---|---|
| Host | `postgres` (DNS interno do Docker) |
| Porta | `5432` |
| Database | `bettacare` |
| Role | `bettacare_user` |
| Senha | no vault, entregue por variável de ambiente |

```
postgresql://bettacare_user:<senha>@postgres:5432/bettacare
```

- **DEVE** receber a string de conexão por **variável de ambiente**, nunca
  embutida na imagem.
- **NÃO** terá acesso de superusuário. A role é dona apenas do próprio database.
- **NÃO** há porta publicada. O banco só é alcançável de dentro da rede
  `backend`.
- Se precisar de alguma **extensão** do PostgreSQL, **INFORMAR qual**:
  extensões são criadas por superusuário e precisam entrar no playbook antes.
- **DEVE** manter o pool de conexões pequeno. São três usuários e poucos acessos
  semanais; um pool grande só consome RAM que o Postgres poderia usar de cache.
- **DEVE** armazenar timestamps em UTC e converter na apresentação. O servidor
  está em `America/Recife`, mas depender do fuso do processo é fonte de bug.

### Migrations

- **DEVE** rodar automaticamente na subida, ou por um comando documentado.
  **INFORMAR qual dos dois.**
- **DEVE** ser idempotente: rodar duas vezes não pode falhar nem duplicar.
- O playbook roda o mesmo compose várias vezes; a aplicação não pode assumir que
  sobe uma vez só.

---

## 6. Configuração, segredos e ciclo de vida

### Variáveis de ambiente

- **DEVE** ler toda a configuração de variáveis de ambiente.
- **DEVE** separar claramente o que é **build time** do que é **runtime**.
  Isso é crítico se o frontend for Next.js: qualquer `NEXT_PUBLIC_*` é
  congelada na imagem durante o build, e precisa ser conhecida pela CI, não pelo
  servidor.
- **NÃO DEVE** haver segredo dentro da imagem. Os segredos vêm do vault do
  Ansible, viram um `.env` com permissão restrita no host, e chegam ao container
  em tempo de execução.
- **INFORMAR** a lista completa de variáveis, com qual é obrigatória, qual tem
  padrão, e qual é segredo.

### Persistência

- **INFORMAR** quais diretórios precisam sobreviver ao container.
- Eles serão montados a partir de `/srv/bettacare/`. Configuração fica em
  `/opt/compose/bettacare/`. As duas árvores têm estratégias de backup
  diferentes e não devem ser misturadas.
- Se a aplicação for stateless além do banco, dizer isso explicitamente -- é o
  cenário mais simples e o preferido.

### Saúde e desligamento

- **DEVE** expor um endpoint de healthcheck. **INFORMAR o path.**
  - Responde `200` quando a aplicação está pronta para servir
  - Não exige autenticação
  - É barato: sem consulta pesada, sem efeito colateral
  - Verifica a conexão com o banco
- **DEVE** subir mesmo se o PostgreSQL ainda não estiver pronto, tentando
  reconectar. Após um reboot do servidor, todos os containers sobem ao mesmo
  tempo e não há garantia de ordem.
- **DEVE** tratar `SIGTERM` encerrando conexões e saindo. Sem isso, todo
  `docker compose down` espera dez segundos e mata o processo à força.
- **DEVE** escrever log em `stdout` e `stderr`. O Docker está configurado com
  driver `json-file`, teto de 10MB e três arquivos por container. Log em arquivo
  dentro do container é perdido e não rotaciona.

### Consumo

- **INFORMAR** a RAM esperada em operação normal e em pico.
- **INFORMAR** se há processo de segundo plano -- worker, cron, polling -- e com
  que frequência roda. Num servidor de 2 núcleos, um job agressivo aparece.

---

## 7. Regras de segurança que não se negociam

Estas vêm de decisões já tomadas e verificadas no servidor. Não são preferências.

### 7.1 Toda porta publicada tem endereço explícito

O Docker escreve direto na chain `DOCKER` do iptables e **ignora o UFW**.
Publicar `- "8080:80"` expõe a porta em todas as interfaces, e o firewall não
opina -- ele protege a chain `INPUT`, enquanto tráfego com DNAT vai para
`FORWARD`.

```yaml
ports:
  - "<IP_DO_SERVIDOR>:8081:80"   # correto
  - "8081:80"                   # NUNCA
```

E como o servidor roteia uma sub-rede de VPN, uma porta sem bind fica acessível
também a toda a rede da VPN, não só à LAN.

### 7.2 A interface web não implementa login próprio

O Cloudflare Access autentica antes da requisição chegar ao servidor. Requisição
não autenticada é barrada na borda e **nunca toca a máquina** -- verificado.

A aplicação **DEVE** aceitar que a identidade vem de fora. O Access injeta o
cabeçalho:

```
Cf-Access-Authenticated-User-Email: pessoa@exemplo.com
```

Use-o para saber quem é o usuário, em vez de construir tela de login,
recuperação de senha e sessão. Menos código, e mais seguro.

**Não confie nesse cabeçalho em requisições que não passaram pelo Access** --
ou seja, no endpoint do ESP32, que é acessível pela LAN. Lá a autenticação é o
token.

### 7.3 Se houver API para consumo por máquina

Se alguma parte precisar ser chamada por outro sistema, sem navegador, o Access
suporta isso com **service tokens**: um par `Client ID` + `Client Secret` enviado
em cabeçalhos `CF-Access-Client-Id` e `CF-Access-Client-Secret`, com uma policy
do tipo `Service Auth`.

A mesma aplicação pode ter as duas policies ao mesmo tempo: `Allow` por e-mail
para humanos, `Service Auth` para máquinas. **INFORMAR** se será necessário.

---

## 8. O que NÃO usar

Decisões já tomadas, com motivo. Reverter exige conversa.

| Não usar | Por quê |
|---|---|
| PHP | A reescrita saiu dele por decisão do usuário |
| MQTT / Mosquitto / HiveMQ | Broker eliminado; ESP32 fala HTTP direto |
| Redis | O volume não justifica cache nem fila |
| MariaDB / MySQL | O cluster PostgreSQL já existe e é compartilhado |
| Caddy / Nginx no stack | O `cloudflared` já roteia; TLS termina na Cloudflare |
| Watchtower | Atualização de container é decisão manual |
| `network_mode: host` | Contorna todo o isolamento e o bind explícito |
| Porta publicada sem endereço | Ver 7.1 |

---

## 9. Checklist do que preciso receber

Para escrever a role e colocar no ar, preciso destas respostas:

- [ ] Repositório, branch e se é privado
- [ ] Comando de build e comando de start
- [ ] **uid com que o processo roda dentro do container**
- [ ] Porta interna da interface web
- [ ] Porta interna do endpoint do ESP32, ou o path a publicar
- [ ] Lista de variáveis de ambiente, separando build time de runtime, e
      marcando quais são segredo
- [ ] Path do healthcheck
- [ ] Migrations: automáticas na subida ou comando separado?
- [ ] Extensões do PostgreSQL, se houver
- [ ] Diretórios que precisam persistir, ou confirmação de que é stateless
- [ ] Contrato do endpoint de telemetria: método, path, corpo, resposta,
      frequência de envio
- [ ] Qual das opções da seção 4.4 foi escolhida para o sentido servidor→ESP32
- [ ] RAM esperada e processos de segundo plano
- [ ] Precisa de service token do Access para consumo por máquina?

Com isso, a implantação segue o roteiro de `docs/nova-aplicacao.md`. Boa parte
do caminho já está pronta para o bettacare: Access configurado, DNS criado,
database e role esperando no cluster.
