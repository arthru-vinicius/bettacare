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
    hint: "Se o maior bloco alocável cair junto, é fragmentação — ver a série de memória em Relatórios.",
  },
  "system.unclean_reset": {
    comp: "system",
    sev: "error",
    label: "Reinício por falha, não comandado",
    hint: "Brownout aponta para alimentação (ver docs/pinagem-e-montagem-esp32.md §1); watchdog aponta para software.",
  },
  "system.ready": {
    comp: "system",
    sev: "info",
    label: "Controle local ativo",
  },
  "system.reboot_requested": {
    comp: "system",
    sev: "warn",
    label: "Reinício solicitado pelo servidor",
  },
  "system.task_failed": {
    comp: "system",
    sev: "fatal",
    label: "Task de rede não iniciou",
    hint: "O dispositivo segue operando sozinho — luz por horário, ventoinha por temperatura — mas sem telemetria nenhuma.",
  },
  "system.wdt_config_failed": {
    comp: "system",
    sev: "error",
    label: "Watchdog da rede não configurou",
    hint: "O prazo ficou no padrão de 5 s: uma operação de rede lenta (NTP, DNS) pode reiniciar o chip. Regrave o firmware.",
  },
  "system.config_loaded": {
    comp: "system",
    sev: "info",
    label: "Configuração carregada do cache local",
  },
  "system.config_applied": {
    comp: "system",
    sev: "info",
    label: "Configuração nova aplicada",
  },

  // ── memória de configuração (NVS) ─────────────────────────────────────────
  "nvs.boot_id_failed": {
    comp: "nvs",
    sev: "warn",
    label: "Contador de boot não persistido",
    hint: "O NVS não abriu para gravação — idempotência do próximo boot pode falhar.",
  },
  "nvs.open_failed": {
    comp: "nvs",
    sev: "warn",
    label: "Falha ao ler configuração salva",
    hint: "Usando os padrões de fábrica até o próximo contato com o servidor.",
  },
  "nvs.no_config": {
    comp: "nvs",
    sev: "info",
    label: "Sem configuração em cache",
  },
  "nvs.write_failed": {
    comp: "nvs",
    sev: "warn",
    label: "Configuração não persistida",
    hint: "Vale para esta sessão, mas não sobrevive a um reboot sem servidor.",
  },
  "nvs.self_test_failed": {
    comp: "nvs",
    sev: "error",
    label: "Memória de configuração não passou no teste",
    hint: "A escrita de teste não voltou íntegra na leitura. A flash pode estar desgastada.",
  },

  // ── atualização remota (OTA) ──────────────────────────────────────────────
  "ota.started": { comp: "ota", sev: "info", label: "Atualização remota iniciada" },
  "ota.succeeded": {
    comp: "ota",
    sev: "info",
    label: "Atualização remota concluída",
  },
  "ota.failed": {
    comp: "ota",
    sev: "error",
    label: "Atualização remota falhou",
    hint: "O dispositivo seguiu com o firmware anterior. Verifique o arquivo enviado e tente de novo.",
  },

  // ── autodiagnóstico ───────────────────────────────────────────────────────
  "system.diagnostic_ran": {
    comp: "system",
    sev: "info",
    label: "Autodiagnóstico executado",
  },
  "system.diagnostic_found_problem": {
    comp: "system",
    sev: "warn",
    label: "Autodiagnóstico encontrou problema",
    hint: "Veja o relatório completo na aba Diagnóstico para saber qual componente falhou.",
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
  "wifi.ap_mode_off": {
    comp: "wifi",
    sev: "info",
    label: "Saiu do modo de recuperação",
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
  "api.server_error": {
    comp: "api",
    sev: "error",
    label: "Servidor inacessível",
    hint: "O aquário continua operando sozinho com a última configuração conhecida.",
  },
  "api.connected": {
    comp: "api",
    sev: "info",
    label: "Primeiro contato com o servidor",
  },
  "api.bad_config": {
    comp: "api",
    sev: "error",
    label: "Configuração recebida é inválida",
    hint: "Mantendo a anterior. Verifique os horários e limiares enviados pelo servidor.",
  },
  "api.unknown_command": {
    comp: "api",
    sev: "warn",
    label: "Comando desconhecido recusado",
    hint: "Firmware desatualizado em relação ao servidor, ou vice-versa.",
  },
  "api.queue_full": {
    comp: "api",
    sev: "error",
    label: "Fila de comandos do dispositivo cheia",
  },
  "api.rejected_body": {
    comp: "api",
    sev: "warn",
    label: "Servidor recusou o corpo enviado",
    hint: "Um campo saiu de faixa. O firmware segue com o próximo ciclo em vez de repetir o mesmo corpo.",
  },
  "ingest.event_dropped": {
    comp: "api",
    sev: "warn",
    label: "Evento do aquário descartado",
    hint: "Um evento veio com um campo que o servidor não reconhece — normalmente firmware mais novo que o servidor. O resto do envio foi aceito.",
  },
  "settings.updated": {
    comp: "system",
    sev: "info",
    label: "Configuração alterada",
  },
  "ingest.field_rejected": {
    comp: "api",
    sev: "warn",
    label: "Campo fora de faixa, corrigido pelo servidor",
    hint: "O resto do envio foi aceito normalmente. Ver o campo específico no contexto do evento.",
  },
  "cmd.late_ack": {
    comp: "api",
    sev: "warn",
    label: "Confirmação chegou depois do prazo",
    hint: "O dispositivo executou o comando, mas o ack chegou depois de expirado. O COMMAND_TTL_S pode estar curto para a rede atual.",
  },

  // ── relógio ────────────────────────────────────────────────────────────
  "rtc.missing": {
    comp: "rtc",
    sev: "error",
    label: "Módulo de relógio não encontrado",
    hint: "O DS3231 não responde no barramento I²C. Verifique as ligações SDA e SCL.",
  },
  "rtc.found": {
    comp: "rtc",
    sev: "info",
    label: "Módulo de relógio reconhecido",
  },
  "rtc.lost_power": {
    comp: "rtc",
    sev: "warn",
    label: "Relógio perdeu a hora",
    hint: "A bateria do DS3231 provavelmente acabou. A hora será ressincronizada por NTP.",
  },
  "rtc.ntp_synced": { comp: "rtc", sev: "info", label: "Hora sincronizada" },
  "rtc.ntp_timeout": {
    comp: "rtc",
    sev: "warn",
    label: "NTP não respondeu",
    hint: "Mantendo a hora do módulo. Verifique a conectividade com a internet.",
  },
  "rtc.automation": {
    comp: "rtc",
    sev: "info",
    label: "Automação por horário",
  },
  "rtc.bad_read": {
    comp: "rtc",
    sev: "warn",
    label: "Leitura inválida do relógio, descartada",
    hint: "O DS3231 devolveu bytes que não formam uma hora. Uma vez é ruído; repetindo, confira SDA, SCL e as soldas do módulo.",
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
  "temp.reset_value": {
    comp: "temp",
    sev: "warn",
    label: "Sensor reiniciou durante a leitura",
    hint: "Devolveu 85,0 °C, o valor de power-on do DS18B20 — leitura descartada. Repetindo, confira a alimentação da sonda (VCC, GND e o 100 nF).",
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
  "fan.tach_ok": {
    comp: "fan",
    sev: "info",
    label: "Ventoinha voltou a girar",
  },
  "fan.tach_implausible": {
    comp: "fan",
    sev: "warn",
    label: "Leitura de RPM implausível, corrigida",
    hint: "O tacômetro contou pulsos demais para ser rotação real — sinal de ruído na linha. Verifique o pull-up.",
  },
  "fan.failsafe": {
    comp: "fan",
    sev: "warn",
    label: "Ventoinha em modo de segurança",
    hint: "A temperatura ficou indisponível; a ventoinha assumiu velocidade fixa por precaução.",
  },
  "fan.failsafe_cleared": {
    comp: "fan",
    sev: "info",
    label: "Temperatura voltou; controle automático retomado",
  },
  "fan.cooldown": { comp: "fan", sev: "info", label: "Ventoinha em cooldown" },
  "fan.escalated": {
    comp: "fan",
    sev: "warn",
    label: "Piso de velocidade elevado",
    hint: "A temperatura não caiu o suficiente no intervalo esperado; a ventoinha subiu um degrau sozinha.",
  },
  "fan.mode_auto": {
    comp: "fan",
    sev: "info",
    label: "Controle automático restaurado",
  },
  "fan.mode_manual": {
    comp: "fan",
    sev: "info",
    label: "Modo manual ativado",
  },
  "fan.pwm_failed": {
    comp: "fan",
    sev: "fatal",
    label: "PWM da ventoinha não configurou",
    hint: "A ventoinha ficou sem controle nenhum. Provável falha do periférico LEDC do ESP32.",
  },
  "fan.power_pin_fault": {
    comp: "fan",
    sev: "error",
    label: "Pino do corte de energia não obedece",
    hint: "O GPIO26 não reflete o que o firmware escreve — o retrato de pino queimado, como aconteceu com o GPIO27. Meça o pino com o multímetro.",
  },
  "fan.power_pin_ok": {
    comp: "fan",
    sev: "info",
    label: "Pino do corte de energia voltou a obedecer",
  },

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
  "button.sampler_failed": {
    comp: "button",
    sev: "fatal",
    label: "Leitura do botão não iniciou",
    hint: "O timer que amostra o botão físico não subiu: ele fica sem efeito até o dispositivo reiniciar.",
  },
  "pot.out_of_range": {
    comp: "pot",
    sev: "warn",
    label: "Potenciômetro fora da faixa",
    hint: "A leitura do ADC saiu do intervalo esperado — provável mau contato.",
  },
  "pot.fan_off": {
    comp: "pot",
    sev: "info",
    label: "Ventoinha desligada pelo potenciômetro",
  },
  "pot.fan_speed": {
    comp: "pot",
    sev: "info",
    label: "Velocidade ajustada pelo potenciômetro",
  },

  // ── alimentador (módulo opcional) ──────────────────────────────────────
  "feeder.module_connected": {
    comp: "feeder",
    sev: "info",
    label: "Módulo do alimentador conectado",
  },
  "feeder.module_disconnected": {
    comp: "feeder",
    sev: "info",
    label: "Módulo do alimentador desconectado",
    hint: "Normal se o módulo não fica ligado o tempo todo — só é problema se você esperava que estivesse.",
  },
  "feeder.fed_ok": {
    comp: "feeder",
    sev: "info",
    label: "Alimentação concluída",
  },
  "feeder.fed_incomplete": {
    comp: "feeder",
    sev: "warn",
    label: "Alimentação incompleta",
    hint: "O sensor confirmou menos grãos do que foi pedido — grão pode ter ficado preso no tubo.",
  },
  "feeder.config_applied": {
    comp: "feeder",
    sev: "info",
    label: "Agenda do alimentador atualizada",
  },
  "feeder.module_offline": {
    comp: "feeder",
    sev: "warn",
    label: "Módulo do alimentador não respondeu",
    hint: "Confira se o módulo está ligado e o conector de 4 vias está encaixado.",
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
  "cmd.stale_discarded": {
    comp: "api",
    sev: "warn",
    label: "Comando descartado por idade",
    hint: "Ficou tempo demais na fila sem o dispositivo aparecer — executar agora seria uma decisão já obsoleta.",
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
  feeder: "Alimentador",
};
