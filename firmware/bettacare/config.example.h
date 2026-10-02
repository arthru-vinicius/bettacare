#pragma once

// =============================================================================
// CONFIGURAÇÃO — copie para config.h e preencha os valores reais.
// config.h está no .gitignore e nunca deve ser versionado: este repositório
// é público.
// =============================================================================
//
// O que mudou em relação ao sistema antigo:
//
//   - Não há mais MQTT. O dispositivo fala HTTP direto com o servidor na LAN.
//   - Horários da luminária e limiares da ventoinha NÃO ficam mais aqui como
//     fonte de verdade. O servidor manda a configuração e o NVS a guarda como
//     cache offline. Os valores abaixo são apenas o que vale antes do primeiro
//     contato com o servidor — e depois de uma limpeza de NVS.
//
// Bibliotecas necessárias (Arduino Library Manager):
//   ArduinoJson >= 7.0   (a API v7 é usada; v6 não compila)
//   RTClib, OneWire, DallasTemperature, ElegantOTA

// --- Identidade do dispositivo -----------------------------------------------
// Precisa casar com o slug usado no servidor: minúsculas, dígitos e hífen.
#define DEVICE_ID      "aquarium-01"
#define FW_VERSION     "2.0.3"

// --- Wi-Fi -------------------------------------------------------------------
#define WIFI_SSID      "YOUR_NETWORK_HERE"
#define WIFI_PASSWORD  "YOUR_PASSWORD_HERE"
#define WIFI_RECONNECT_INTERVAL_MS  10000UL   // tentativa de reconexão (não-bloqueante)
#define WIFI_RECOVERY_TIMEOUT_MS    180000UL  // após esse tempo offline, habilita AP de recuperação
#define WIFI_RECOVERY_MAX_ATTEMPTS  12        // ou após esse número de tentativas sem sucesso
#define WIFI_RECOVERY_AP_SSID_PREFIX "BettaCare-Setup"

// --- Rede (IP estático ou DHCP) -----------------------------------------------
// "0.0.0.0" usa DHCP. Com o servidor nunca iniciando conexão, o IP do ESP32
// deixou de importar — DHCP é a escolha certa agora.
// Com DHCP ("0.0.0.0") os dois campos abaixo são ignorados — e DHCP é a escolha
// certa agora, porque o servidor nunca inicia conexão com o dispositivo.
#define NET_LOCAL_IP   "0.0.0.0"
#define NET_GATEWAY    "REPLACE_WITH_GATEWAY_IP"
#define NET_SUBNET     "255.255.255.0"

// --- Servidor BettaCare ------------------------------------------------------
// Porta de ingestão, publicada com bind explícito no IP da LAN do servidor.
// HTTP puro: é tráfego local e o custo de TLS num ESP32 não se paga aqui.
// Placeholder de propósito: um IP plausível aqui passaria despercebido e o
// dispositivo falharia em silêncio contra um endereço que não é o seu.
#define SERVER_HOST         "REPLACE_WITH_SERVER_IP"
#define SERVER_PORT         8080
#define SERVER_TELEMETRY_PATH "/api/v1/telemetry"

// Precisa ser idêntico ao DEVICE_INGEST_TOKEN do servidor.
#define SERVER_API_TOKEN    "REPLACE_WITH_THE_SAME_TOKEN_AS_THE_SERVER"

// Timeout de uma requisição inteira. Roda na task de rede, então bloquear aqui
// não afeta o botão físico nem a histerese da ventoinha.
#define HTTP_TIMEOUT_MS     4000

// --- Padrões usados antes do primeiro contato com o servidor -----------------
// Depois do primeiro POST bem-sucedido, quem manda é o servidor.
#define DEFAULT_LIGHT_ON_HOUR    10
#define DEFAULT_LIGHT_ON_MIN      0
#define DEFAULT_LIGHT_OFF_HOUR   17
#define DEFAULT_LIGHT_OFF_MIN     0
#define DEFAULT_FAN_TRIGGER_C   29.0f
#define DEFAULT_FAN_OFF_C       27.5f
#define DEFAULT_TELEMETRY_INTERVAL_MS 3000
#define DEFAULT_HEARTBEAT_INTERVAL_MS 60000

// --- Pinos de hardware -------------------------------------------------------
#define PIN_SSR        23   // Relé SSR40DA — luminária LED
#define PIN_DS18B20    19   // DS18B20 (OneWire)
#define PIN_BUTTON     18   // Push button (INPUT_PULLUP)
#define PIN_POT        34   // Potenciômetro B10K (ADC)
#define PIN_FAN        17   // Fan 4 pinos, pino 4 — PWM 25 kHz
#define PIN_FAN_TACH   25   // Fan 4 pinos, pino 3 — tacômetro (open-drain)
#define PIN_FAN_POWER  26   // Corte de energia da ventoinha: AO3400 no retorno (GND); Tipo A, nunca para só por PWM
// GPIO27 era o pino original — queimou (provável curto gate/dreno do AO3400
// durante a montagem, expondo o pino a 12V). NÃO reutilize o GPIO27 pra nada.

// DS3231SN usa o I2C padrão do ESP32: SDA = GPIO 21, SCL = GPIO 22

// --- Módulo opcional de alimentação de precisão (UART2) ----------------------
// Ver pinagem-e-montagem-esp32.md §8 e pinagem-alimentador-modulo.md.
// Não são os pinos "padrão" 17/16 da UART2 porque o 17 já é o PWM da
// ventoinha — ver a mesma nota no doc de pinagem.
#define PIN_FEEDER_TX     4   // TXD2 — para o RX do módulo
#define PIN_FEEDER_RX     16  // RXD2 — do TX do módulo
#define FEEDER_LINK_BAUD  9600

// --- NTP ---------------------------------------------------------------------
// Fuso: Brasília = UTC-3 (sem horário de verão desde 2019).
// O RTC guarda hora local; o servidor carimba tudo em UTC do lado dele.
#define NTP_SERVER1      "pool.ntp.org"
#define NTP_SERVER2      "time.google.com"
#define NTP_UTC_OFFSET   (-3 * 3600)

// --- Controle automático da ventoinha ----------------------------------------
// Estes NÃO vêm do servidor: são a forma da curva, não a política. O servidor
// controla apenas os dois limiares de temperatura.
#define FAN_COOLDOWN_MIN    30      // minutos de funcionamento após atingir o limiar de desligar
#define FAN_SPEED_LOW       30      // Δ ≤ 1,0 °C acima do gatilho
#define FAN_SPEED_MED       55      // Δ ≤ 2,0 °C
#define FAN_SPEED_HIGH      80      // Δ ≤ 3,0 °C
#define FAN_SPEED_MAX       100     // Δ >  3,0 °C
#define FAN_FAILSAFE_SPEED  30      // usado no AUTO quando a temperatura fica indisponível

// Escalonamento progressivo: se após FAN_ESCALATION_INTERVAL_MIN minutos em
// RUNNING a temperatura não tiver caído FAN_ESCALATION_DROP_C graus, o piso de
// velocidade avança um degrau. Zerado no cooldown e na transição para o período ON.
#define FAN_ESCALATION_INTERVAL_MIN  10
#define FAN_ESCALATION_DROP_C        0.5f

// Leitura de temperatura mais velha que isto deixa de valer como estado atual.
//
// Precisa ser confortavelmente maior que o intervalo de amostragem do
// DS18B20 (5 s, ver SAMPLE_INTERVAL_MS em temperature.cpp) somado ao tempo de
// conversão. Com 5 s de teto, uma leitura recém-espacada apareceria como
// "velha" e dispararia o failsafe da ventoinha sem nenhum defeito real.
// 15 s dá margem de quase 3x. O servidor usa 30 s, ainda mais folgado.
#define TEMP_MAX_STALE_MS   15000UL

// Faixa fisicamente possível da água nesta instalação (Recife, com
// ar-condicionado). Fora dela a leitura é descartada como defeito do fio
// (`temp.implausible`) — foi um -48,00 °C isolado em produção. Precisa
// casar com `TEMP_PLAUSIBLE_C` do contrato, que o servidor usa. Sem as
// linhas, valem 10 e 45.
// #define TEMP_PLAUSIBLE_MIN_C 10.0f
// #define TEMP_PLAUSIBLE_MAX_C 45.0f

// --- API local do ESP32 e OTA ------------------------------------------------
// Continua existindo para diagnóstico direto e recuperação, independente do
// servidor. Interface de atualização em http://<IP_DO_ESP32>/update
#define API_AUTH_TOKEN      "REPLACE_WITH_LONG_RANDOM_TOKEN"
#define OTA_USERNAME        "admin"
#define OTA_PASSWORD        "REPLACE_WITH_STRONG_PASSWORD"

// ATENÇÃO: a senha de OTA do repositório antigo esteve versionada em texto
// claro e deve ser considerada comprometida. Ao gravar este firmware, use uma
// senha nova — não reaproveite a anterior.
