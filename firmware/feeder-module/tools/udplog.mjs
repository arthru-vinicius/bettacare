// Receptor do log do módulo alimentador, para a bancada sem o cabo USB.
//
//   MODO=udp  (padrão)  ouve os datagramas que o módulo manda quando o
//                       config.h dele tem DEBUG_LOG_HOST apontando para este
//                       PC; porta PORTA, padrão 5514
//   MODO=http           busca GET /log no módulo a cada 2 s (MODULO=http://<ip>),
//                       com o login do OTA lido do config.h montado em /config.h
//                       — para quando o firewall do PC barra a entrada de UDP
//
// Cada linha sai com a hora de Brasília na frente; `docker logs -f` acompanha.
// Comandos no README.md do firmware, seção "Depuração pela rede".
import dgram from "node:dgram";
import { readFileSync } from "node:fs";

const MODO = process.env.MODO ?? "udp";
const hora = () => new Date(Date.now() - 3 * 3600 * 1000).toISOString().slice(11, 19);
const imprime = (texto) => process.stdout.write(`${hora()} ${texto}\n`);

function ouveUdp() {
  const porta = Number(process.env.PORTA ?? 5514);
  const socket = dgram.createSocket("udp4");
  socket.on("message", (msg, origem) => imprime(`${origem.address} ${msg.toString("utf8").trimEnd()}`));
  socket.bind(porta, () => imprime(`[receptor] ouvindo UDP ${porta}`));
}

function buscaHttp() {
  const modulo = process.env.MODULO;
  if (!modulo) throw new Error("MODO=http precisa de MODULO=http://<ip do módulo>");
  // O login vem do config.h montado, nunca da linha de comando: lá ele
  // apareceria no `docker inspect` e no histórico do terminal.
  const config = readFileSync("/config.h", "utf8");
  const valor = (nome) => config.match(new RegExp(`^#define\\s+${nome}\\s+"([^"]*)"`, "m"))?.[1] ?? "";
  const auth = `Basic ${Buffer.from(`${valor("OTA_USERNAME")}:${valor("OTA_PASSWORD")}`).toString("base64")}`;

  let ultimo = 0;
  let fora = false;
  const busca = async () => {
    try {
      const resposta = await fetch(`${modulo}/log?desde=${ultimo}`, {
        headers: { authorization: auth },
        signal: AbortSignal.timeout(5000),
      });
      if (fora) imprime("[receptor] módulo de volta");
      fora = false;
      if (!resposta.ok) {
        imprime(`[receptor] HTTP ${resposta.status}`);
        return;
      }
      // A próxima linha do módulo com número menor que a última lida: ele
      // ficou sem energia e o log recomeçou do 1.
      const proxima = Number(resposta.headers.get("x-log-proxima") ?? 0);
      if (proxima > 0 && proxima <= ultimo) {
        imprime("[receptor] o log do módulo recomeçou (faltou energia)");
        ultimo = 0;
        return;
      }
      for (const linha of (await resposta.text()).split("\n")) {
        if (!linha) continue;
        const seq = Number.parseInt(linha, 10);
        if (seq > ultimo) ultimo = seq;
        imprime(linha);
      }
    } catch (erro) {
      if (!fora) imprime(`[receptor] sem resposta do módulo (${erro.message}); tentando a cada 2 s`);
      fora = true;
    }
  };
  imprime(`[receptor] buscando ${modulo}/log a cada 2 s`);
  busca();
  setInterval(busca, 2000);
}

if (MODO === "udp") ouveUdp();
else buscaHttp();
