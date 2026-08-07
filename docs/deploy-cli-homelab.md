# `homelab update` — especificação para o agente do homelab

Proposta de uma CLI no `PATH` do servidor que troca a versão publicada de um
serviço. Escrito do lado do BettaCare: descreve o que o serviço **oferece** e o
que ele **precisa**, não como implementar.

---

## A ideia em uma frase

O trabalho da ferramenta **não é "atualizar"**. É *trocar a versão fixada e
provar que o serviço continua de pé* — e desfazer sozinha se não continuar.

Essa distinção decide todo o resto do desenho. Um script que roda
`docker compose up -d` e imprime "pronto" não é isso: ele reporta que o Docker
aceitou o comando, não que a aplicação subiu.

## Por que esta forma, e não as outras

Foram consideradas três alternativas e todas perdem para esta:

| Alternativa | Por que não |
|---|---|
| Runner self-hosted do GitHub | O repositório é **público**. Um PR de terceiro pode escolher rodar no runner e executar código dentro da LAN. O próprio GitHub desaconselha |
| Actions com SSH pelo túnel | Funciona, mas coloca chave de acesso ao servidor num segredo do GitHub para economizar um comando |
| Timer que verifica sozinho | É a forma do Watchtower, proibido no stack |

A CLI local não expõe porta nenhuma, não guarda segredo em lugar nenhum e não
depende de o GitHub estar de pé. **E não conflita com a proibição do
Watchtower**, que é sobre mutação não supervisionada: aqui a atualização é
explícita, versionada e iniciada por uma pessoa. Vale deixar isso registrado
para a proposta não ser recusada por semelhança superficial.

O custo é continuar dependendo de um comando manual — que é exatamente o que se
quer manter.

---

## A mudança estrutural que faz o resto funcionar

Hoje a tag vive dentro do `docker-compose.yml`. **Ela precisa sair de lá.**

```yaml
# /opt/compose/bettacare/docker-compose.yml
services:
  bettacare:
    image: ghcr.io/arthru-vinicius/bettacare:${BETTACARE_TAG:?defina BETTACARE_TAG}
```

```bash
# /opt/compose/bettacare/.env
BETTACARE_TAG=v1.0.2
```

Três ganhos, e o primeiro sozinho já justifica:

1. A ferramenta edita **uma linha de `KEY=valor`** em vez de aplicar expressão
   regular em YAML. Editar YAML com `sed` é como ferramenta de deploy quebra —
   basta um comentário, uma aspa ou uma segunda ocorrência de `image:`.
2. `${BETTACARE_TAG:?}` faz o compose **falhar alto** se a variável sumir, em
   vez de silenciosamente tentar `:latest`.
3. A versão em produção vira um fato consultável: `grep BETTACARE_TAG .env`.

Convenção sugerida para os demais serviços: `<NOME>_TAG` no `.env` ao lado do
compose. Assim a ferramenta funciona por convenção, sem cadastro central.

---

## O que o comando faz, em ordem

```
homelab update bettacare            # mostra o que mudaria e pede confirmação
homelab update bettacare v1.0.2     # versão explícita
homelab update bettacare -y         # sem perguntar
homelab status                      # o que roda vs. o que há de novo
homelab rollback bettacare          # volta à anterior
```

1. **Trava** por serviço (`flock`). Duas invocações simultâneas não podem
   disputar o mesmo compose.
2. **Descobre as versões** disponíveis (ver abaixo) e escolhe a maior por
   semver — não a mais recente por data.
3. **Mostra o diff** — `v1.0.1 → v1.0.2` — e confirma, salvo `-y`.
4. **`docker compose pull`** antes de qualquer coisa. Falha aqui não derruba o
   que está rodando; falha depois, sim.
5. **Guarda a tag anterior** (`BETTACARE_TAG_ANTERIOR`) e grava a nova.
6. **`docker compose up -d`**.
7. **Verifica a saúde de verdade** — e é este o passo que importa.
8. **Reverte sozinha** se a verificação não passar: restaura a tag anterior,
   sobe de novo, e sai com código diferente de zero.

---

## As quatro decisões que não são óbvias

### 1. Ordenar por semver, nunca por data

A lista de tags do registro vem misturada e fora de ordem. Verificado hoje:

```json
["v1.0.0", "sha-27ffac06…", "v1.0.1", "sha-77366fa3…", "v1.0.2", "sha-a4596fe7…"]
```

O pipeline publica **duas** tags por release: `v<semver>` e `sha-<commit>`. A
ferramenta precisa descartar tudo que não casar com `^v\d+\.\d+\.\d+$` e ordenar
por versão. Ordenar por data de push erra quando um hotfix de ramo antigo sai
depois de uma versão maior.

### 2. `up -d` retornando 0 não significa que subiu

Significa que o Docker aceitou. O container pode reiniciar em laço logo depois —
uma migration que falha, uma variável faltando, o Postgres ainda subindo.

O BettaCare expõe **`GET /healthz` sem autenticação**, e a ferramenta deve
esperar `200` com prazo (sugestão: 60 s, verificando a cada 2 s). Enquanto o
banco não estiver pronto ele responde `503` de propósito — então `503` é
"aguarde", e o prazo é que decide desistir.

Vale também confirmar que o container não está em `restarting`:

```bash
docker inspect -f '{{.State.Status}} {{.RestartCount}}' <container>
```

### 3. Reverter é o que torna o resto confiável

Guardar a tag anterior custa uma linha e transforma o pior caso de "aquário
fora do ar até eu descobrir o que houve" em trinta segundos.

**Uma ressalva honesta:** reverter o *container* é seguro; reverter *através de
uma migration destrutiva* não é. As migrations do BettaCare até aqui só
adicionam coluna ou alargam tipo — nenhuma descarta dado, então voltar uma
versão é seguro **hoje**. A ferramenta não tem como saber disso sozinha.

Dado que o banco é limitado a 1 GB por desenho, um `pg_dump` antes de atualizar
é barato e resolve a ressalva de vez. Recomendo embutir.

### 4. Sem argumento, não faz nada

`homelab update` sozinho deve **listar**, nunca atualizar tudo. O valor da
ferramenta está em atualizar só o que se quer, e um comando que atualiza a casa
inteira por engano é justamente o que se está evitando.

---

## O que o BettaCare oferece

| | |
|---|---|
| Imagem | `ghcr.io/arthru-vinicius/bettacare:v<semver>` |
| Visibilidade | **pública** — nenhum segredo necessário para consultar ou baixar |
| Saúde | `GET /healthz` nas duas portas, sem autenticação |
| Migrations | aplicadas na subida, idempotentes, só aditivas até a `0003` |
| Reinício | `SIGTERM` encerra em ~540 ms |
| Nunca publica | tag `latest` |

Listagem de tags sem credencial — testado hoje, funciona por o pacote ser
público:

```bash
TOKEN=$(curl -s "https://ghcr.io/token?scope=repository:arthru-vinicius/bettacare:pull&service=ghcr.io" \
        | jq -r .token)
curl -s -H "Authorization: Bearer $TOKEN" \
     https://ghcr.io/v2/arthru-vinicius/bettacare/tags/list | jq -r '.tags[]'
```

Se o repositório algum dia virar privado, isto passa a exigir um PAT com
`read:packages` — e aí a decisão sobre o vault volta à mesa.

---

## O que não fazer

- **Não** usar `latest`, nem como padrão nem como reserva.
- **Não** editar o `docker-compose.yml` com expressão regular.
- **Não** tratar saída 0 do `up -d` como sucesso.
- **Não** atualizar serviço nenhum sem ser nomeado.
- **Não** rodar como `root` além do necessário para falar com o socket do
  Docker.

---

## Como fica o ciclo, no fim

Do meu lado, nada muda: `git tag v1.0.3 && git push origin v1.0.3`, e o Actions
compila, testa contra um Postgres real e publica.

Do seu lado, o que hoje é entrar no servidor, abrir o compose, trocar a tag e
subir, vira:

```
homelab update bettacare
```

E se der errado, ele volta sozinho antes de você perceber.
