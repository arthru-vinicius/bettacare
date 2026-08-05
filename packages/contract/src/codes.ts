import type { Component, Severity } from "./primitives.js";

/**
 * Catálogo dos códigos de evento conhecidos.
 *
 * Serve para a interface traduzir `light.gpio_fault` em algo que um humano
 * entenda, e para dar um próximo passo quando existe um. Um código ausente
 * daqui **não é erro**: é gravado igual e exibido com o rótulo cru. Firmware
 * novo pode inventar códigos sem esperar por um deploy do servidor.
 */
export interface EventCodeInfo {
  comp: Component;
  sev: Severity;
  /** Texto curto, em português, para a lista de logs. */
  label: string;
  /** O que fazer a respeito. Só quando existe uma ação concreta. */
  hint?: string;
}

export const EVENT_CODES = {
  // ── sistema ────────────────────────────────────────────────────────────
  "system.boot": { comp: "system", sev: "info", label: "Dispositivo iniciou" },
  "system.panic": {
    comp: "system",
    sev: "fatal",
    label: "Reinício por falha grave",
    hint: "O ESP32 reiniciou sozinho. Verifique a alimentação e o histórico de temperatura.",
  },
  "system.event_overflow": {
    comp: "system",
    sev: "warn",
    label: "Eventos descartados por excesso",
    hint: "Algum componente está gerando eventos em rajada — veja qual está em falha.",
  },
  "system.low_memory": {
    comp: "system",
    sev: "warn",
    label: "Memória livre baixa",
  },

  // ── rede ───────────────────────────────────────────────────────────────
  "wifi.connected": { comp: "wifi", sev: "info", label: "Wi-Fi conectado" },
  "wifi.disconnected": {
    comp: "wifi",
    sev: "warn",
    label: "Wi-Fi caiu",
  },
  "wifi.weak_signal": {
    comp: "wifi",
    sev: "warn",
    label: "Sinal de Wi-Fi fraco",
    hint: "RSSI abaixo de -80 dBm. Aproxime o roteador ou o dispositivo.",
  },
  "wifi.ap_mode": {
    comp: "wifi",
    sev: "warn",
    label: "Entrou em modo de recuperação",
    hint: "O dispositivo abriu o próprio ponto de acesso para reconfiguração das credenciais.",
  },

  // ── comunicação com o servidor ─────────────────────────────────────────
  "api.post_failed": {
    comp: "api",
    sev: "warn",
    label: "Falha ao enviar telemetria",
  },
  "api.unauthorized": {
    comp: "api",
    sev: "error",
    label: "Token de ingestão recusado",
    hint: "O X-Api-Token do firmware não bate com o DEVICE_INGEST_TOKEN do servidor.",
  },
  "api.server_unreachable": {
    comp: "api",
    sev: "error",
    label: "Servidor inacessível",
    hint: "O aquário continua operando sozinho com a última configuração conhecida.",
  },

  // ── relógio ────────────────────────────────────────────────────────────
  "rtc.missing": {
    comp: "rtc",
    sev: "error",
    label: "Módulo de relógio não encontrado",
    hint: "O DS3231 não responde no barramento I²C. Verifique as ligações SDA e SCL.",
  },
  "rtc.lost_power": {
    comp: "rtc",
    sev: "warn",
    label: "Relógio perdeu a hora",
    hint: "A bateria do DS3231 provavelmente acabou. A hora será ressincronizada por NTP.",
  },
  "rtc.ntp_synced": { comp: "rtc", sev: "info", label: "Hora sincronizada" },
  "rtc.automation": {
    comp: "rtc",
    sev: "info",
    label: "Automação por horário",
  },

  // ── temperatura ────────────────────────────────────────────────────────
  "temp.sensor_lost": {
    comp: "temp",
    sev: "error",
    label: "Sensor de temperatura sumiu",
    hint: "Nenhum DS18B20 responde no barramento 1-Wire. Verifique o cabo e o resistor de pull-up.",
  },
  "temp.sensor_found": {
    comp: "temp",
    sev: "info",
    label: "Sensor de temperatura reconhecido",
  },
  "temp.stale": {
    comp: "temp",
    sev: "warn",
    label: "Leitura de temperatura desatualizada",
  },
  "temp.crc_error": {
    comp: "temp",
    sev: "warn",
    label: "Leitura corrompida",
    hint: "CRC inválido — normalmente cabo longo demais ou mau contato.",
  },
  "temp.out_of_range": {
    comp: "temp",
    sev: "error",
    label: "Temperatura fora da faixa segura",
  },

  // ── ventoinha ──────────────────────────────────────────────────────────
  "fan.on": { comp: "fan", sev: "info", label: "Ventoinha ligou" },
  "fan.off": { comp: "fan", sev: "info", label: "Ventoinha desligou" },
  "fan.tach_stalled": {
    comp: "fan",
    sev: "error",
    label: "Ventoinha não está girando",
    hint: "Há sinal de PWM mas o tacômetro lê zero. Cabo solto, rolamento travado ou fonte caída.",
  },
  "fan.failsafe": {
    comp: "fan",
    sev: "warn",
    label: "Ventoinha em modo de segurança",
    hint: "A temperatura ficou indisponível; a ventoinha assumiu velocidade fixa por precaução.",
  },
  "fan.cooldown": { comp: "fan", sev: "info", label: "Ventoinha em cooldown" },

  // ── luminária ──────────────────────────────────────────────────────────
  "light.on": { comp: "light", sev: "info", label: "Luminária acendeu" },
  "light.off": { comp: "light", sev: "info", label: "Luminária apagou" },
  "light.gpio_fault": {
    comp: "light",
    sev: "error",
    label: "Falha ao acionar a luminária",
    hint: "O comando foi aceito mas o estado não mudou. Suspeite do SSR ou da alimentação da lâmpada.",
  },
  "light.state_mismatch": {
    comp: "light",
    sev: "error",
    label: "Luminária não obedeceu ao comando",
    hint: "O estado reportado divergiu do comandado por dois ciclos seguidos.",
  },

  // ── entradas físicas ───────────────────────────────────────────────────
  "button.pressed": { comp: "button", sev: "info", label: "Botão pressionado" },
  "button.stuck": {
    comp: "button",
    sev: "warn",
    label: "Botão parece preso",
    hint: "Sinal de pressionado por mais de 30 segundos. Verifique o contato.",
  },
  "pot.out_of_range": {
    comp: "pot",
    sev: "warn",
    label: "Potenciômetro fora da faixa",
    hint: "A leitura do ADC saiu do intervalo esperado — provável mau contato.",
  },

  // ── comandos (gerados pelo servidor) ───────────────────────────────────
  "cmd.queued": { comp: "api", sev: "info", label: "Comando enfileirado" },
  "cmd.acked": { comp: "api", sev: "info", label: "Comando confirmado" },
  "cmd.rejected": {
    comp: "api",
    sev: "error",
    label: "Comando recusado pelo dispositivo",
  },
  "cmd.expired": {
    comp: "api",
    sev: "error",
    label: "Comando expirou sem confirmação",
    hint: "O dispositivo recebeu o comando mas nunca confirmou. Pode ter reiniciado no meio.",
  },
  "cmd.device_offline": {
    comp: "api",
    sev: "error",
    label: "Dispositivo offline — comando na fila",
    hint: "O comando será entregue assim que o ESP32 voltar a falar com o servidor.",
  },

  // ── servidor ───────────────────────────────────────────────────────────
  "device.offline": {
    comp: "system",
    sev: "error",
    label: "Dispositivo parou de responder",
  },
  "device.online": {
    comp: "system",
    sev: "info",
    label: "Dispositivo voltou",
  },
  "db.partition_dropped": {
    comp: "system",
    sev: "info",
    label: "Dados antigos descartados pelo limite de tamanho",
  },
  "db.size_limit_unreachable": {
    comp: "system",
    sev: "warn",
    label: "Limite de tamanho do banco não pôde ser respeitado",
    hint: "Só resta a partição do mês corrente e ela sozinha excede o teto. Aumente DB_SIZE_LIMIT_BYTES.",
  },
} as const satisfies Record<string, EventCodeInfo>;

export type KnownEventCode = keyof typeof EVENT_CODES;

/** Traduz um código para exibição, tolerando códigos que ainda não conhecemos. */
export function describeEventCode(code: string): EventCodeInfo | undefined {
  const known = (EVENT_CODES as Record<string, EventCodeInfo>)[code];
  if (known) return known;

  // O servidor sintetiza `<componente>.recovered` ao registrar a volta de um
  // componente ao normal. São onze combinações possíveis e todas dizem a mesma
  // coisa — descrever a família custa menos que onze entradas no catálogo.
  const recovered = /^([a-z]+)\.recovered$/.exec(code);
  if (recovered) {
    const comp = recovered[1] as Component;
    return {
      comp,
      sev: "info",
      label: `${COMPONENT_LABELS_SHORT[comp] ?? comp} voltou ao normal`,
    };
  }

  return undefined;
}

/** Nomes curtos, para caber na frase de um rótulo de log. */
const COMPONENT_LABELS_SHORT: Partial<Record<Component, string>> = {
  system: "Sistema",
  wifi: "Wi-Fi",
  api: "Comunicação",
  rtc: "Relógio",
  temp: "Sensor de temperatura",
  fan: "Ventoinha",
  light: "Luminária",
  button: "Botão",
  pot: "Potenciômetro",
};
