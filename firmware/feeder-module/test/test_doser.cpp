// O ciclo de uma refeição contra um hardware simulado: reservatório com N
// grãos, feixe que vê (ou não) o grão cair, servo, M1 e M2. Inclui o .cpp real.
#include <string.h>

#include "check.h"
#include "doser.cpp"

// ── Hardware simulado ────────────────────────────────────────────────────────
static uint32_t g_ms = 1000;
static int  g_hopper = 100;          // grãos no reservatório
static bool g_sensor_works = true;   // o LED IR e o fototransistor funcionam
static bool g_led = false;
static int  g_blocked = -1;          // nível "bloqueado" armado; -1 = desarmado
static uint16_t g_count = 0;
static bool g_attached = false;
static uint8_t g_angle = 40;         // no boot, o slide está no repouso
static uint8_t g_cal_dump = 100;
static int  g_dumps_seen = 0;        // vezes que o bolso chegou ao tubo
static uint8_t g_m1 = 0, g_m2 = 0, g_m2_max = 0;
static uint32_t g_m1_on_since = 0, g_m1_longest = 0;
static bool g_m2_before_led = false;
static bool g_fresh = false;         // a próxima escrita é a primeira depois de prender o servo
static int  g_attach_jump = 0;       // salto da primeira escrita: o slide estava em g_angle
static int  g_max_jump = 0;          // maior salto entre escritas seguidas

uint32_t hw_millis() { return g_ms; }
void hw_servo_attach() {
  g_attached = true;
  g_fresh = true;
}
void hw_servo_detach() { g_attached = false; }
void hw_servo_write_deg(uint8_t deg) {
  int salto = deg > g_angle ? deg - g_angle : g_angle - deg;
  if (g_fresh) {
    if (salto > g_attach_jump) g_attach_jump = salto;
    g_fresh = false;
  } else if (salto > g_max_jump) {
    g_max_jump = salto;
  }
  bool chegou = deg == g_cal_dump && g_angle != g_cal_dump;
  g_angle = deg;
  if (!chegou) return;
  g_dumps_seen++;
  if (g_hopper > 0) {
    g_hopper--;
    if (g_sensor_works && g_blocked >= 0) g_count++;
  }
}
void hw_m1_duty(uint8_t pct) {
  if (pct > 0 && g_m1 == 0) g_m1_on_since = g_ms;
  if (pct == 0 && g_m1 > 0 && g_ms - g_m1_on_since > g_m1_longest) g_m1_longest = g_ms - g_m1_on_since;
  g_m1 = pct;
}
void hw_m2_duty(uint8_t pct) {
  g_m2 = pct;
  if (pct > g_m2_max) g_m2_max = pct;
  if (pct > 0 && !g_led) g_m2_before_led = true;
}
void hw_ir_led(bool on) { g_led = on; }
// LM393: apagado (ou feixe cortado) = 1; aceso e livre = 0. Quebrado: sempre 1.
int hw_ir_level() { return (g_sensor_works && g_led) ? 0 : 1; }
void hw_ir_arm(int blocked) { g_blocked = blocked; g_count = 0; }
void hw_ir_disarm() { g_blocked = -1; }
uint16_t hw_ir_take_count() { uint16_t c = g_count; g_count = 0; return c; }
void hw_status_led(bool) {}

static void reset(int hopper, bool sensor) {
  g_hopper = hopper;
  g_sensor_works = sensor;
  g_led = false;
  g_blocked = -1;
  g_count = 0;
  g_dumps_seen = 0;
  g_m2_max = 0;
  g_m1_longest = 0;
  g_m2_before_led = false;
  g_attach_jump = 0;
  g_max_jump = 0;
}

/** Roda o loop por `ms`, de 1 em 1 ms — para o que não deixa o doseador ocupado. */
static void espera(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) {
    g_ms++;
    doser_update();
  }
}

/** Roda o loop de 1 em 1 ms até a refeição acabar (ou um teto de 2 min). */
static uint32_t roda() {
  uint32_t inicio = g_ms;
  while (doser_busy() && g_ms - inicio < 120000) {
    g_ms++;
    doser_update();
  }
  return g_ms - inicio;
}

static const Calibration CAL = {40, 100, true};

int main() {
  MealResult r;

  printf("D1 refeicao normal de 5 graos\n");
  reset(100, true);
  doser_begin_meal(5, Origin::AGENDA, CAL);
  uint32_t dur = roda();
  CHECK(doser_take_result(r), "resultado");
  CHECK(r.requested == 5 && r.confirmed == 5 && r.ok && r.reason == MealReason::OK, "5 de 5");
  CHECK(r.origin == Origin::AGENDA, "origem");
  CHECK(g_dumps_seen == 5 && g_hopper == 95, "um movimento por grao");
  CHECK(g_m2_max == M2_DUTY_PCT && g_m2 == 0, "aviso no peixe, e M2 desligado no fim");
  CHECK(g_m2_before_led, "o aviso vem antes do autoteste");
  CHECK(!g_attached && g_angle == CAL.rest_deg, "slide em repouso e servo solto");
  CHECK(g_attach_jump == 0 && g_max_jump <= SERVO_STEP_DEG, "servo sem tranco: parte de onde esta, 5 graus por vez");
  CHECK(!g_led && g_m1 == 0, "LED IR e M1 desligados");
  CHECK(dur > 4000 && dur < 15000, "~1 s por grao, mais o aviso");
  CHECK(!doser_take_result(r), "o resultado sai uma vez so");

  printf("D2 reservatorio vazio: para depois de 3 movimentos sem grao\n");
  reset(0, true);
  doser_begin_meal(5, Origin::BOTAO, CAL);
  roda();
  CHECK(doser_take_result(r) && r.reason == MealReason::VAZIO && r.confirmed == 0 && !r.ok, "VAZIO");
  CHECK(g_dumps_seen == EMPTY_TRIES, "3 tentativas");
  CHECK(g_m1_longest <= M1_MAX_MS, "M1 dentro do teto");

  printf("D3 acaba no meio: 2 de 5, depois VAZIO\n");
  reset(2, true);
  doser_begin_meal(5, Origin::APP, CAL);
  roda();
  CHECK(doser_take_result(r) && r.confirmed == 2 && r.reason == MealReason::VAZIO, "2 e VAZIO");

  printf("D4 sensor quebrado: conta pelo servo, sem repetir\n");
  reset(100, false);
  doser_begin_meal(5, Origin::AGENDA, CAL);
  roda();
  CHECK(doser_take_result(r) && r.reason == MealReason::SENSOR && r.confirmed == 0 && !r.ok, "SENSOR");
  CHECK(g_dumps_seen == 5 && g_hopper == 95, "5 movimentos, 5 graos");
  CHECK(!g_led, "LED IR apagado");

  printf("D5 so o aviso (3x): sem servo, sem racao, sem resultado\n");
  reset(100, true);
  doser_begin_warning_test();
  roda();
  CHECK(!doser_take_result(r), "sem resultado");
  CHECK(g_dumps_seen == 0 && g_hopper == 100 && g_m2_max == M2_DUTY_PCT && g_m2 == 0, "so o M2");

  printf("D6 pulso diario do M1, com teto\n");
  reset(100, true);
  doser_begin_m1_pulse(2000);
  roda();
  CHECK(g_m1 == 0 && g_m1_longest >= 1990 && g_m1_longest <= 2010, "2 s");
  doser_begin_m1_pulse(60000);
  roda();
  CHECK(g_m1_longest <= M1_MAX_MS + 5, "pedido de 60 s vira o teto");

  printf("D7 abortar no meio: tudo desligado, sem resultado\n");
  reset(100, true);
  doser_begin_meal(5, Origin::AGENDA, CAL);
  for (int i = 0; i < 6000; i++) {
    g_ms++;
    doser_update();
  }
  doser_abort();
  CHECK(!doser_busy() && !g_attached && !g_led && g_m1 == 0 && g_m2 == 0, "estado seguro");
  CHECK(!doser_take_result(r), "sem resultado");

  printf("D8 ocupado: um segundo pedido no meio e ignorado\n");
  reset(100, true);
  doser_begin_meal(3, Origin::AGENDA, CAL);
  doser_begin_meal(9, Origin::APP, CAL);
  roda();
  CHECK(doser_take_result(r) && r.requested == 3 && r.origin == Origin::AGENDA, "vale o primeiro");

  printf("D9 abortado no meio do curso: o proximo movimento parte dali\n");
  reset(100, true);
  doser_begin_meal(5, Origin::AGENDA, CAL);
  uint32_t limite = g_ms + 20000;
  while ((doser_phase() != DoserPhase::DOSING || g_angle <= CAL.rest_deg + 10) && g_ms < limite) {
    g_ms++;
    doser_update();
  }
  doser_abort();
  uint8_t parou_em = g_angle;
  CHECK(parou_em > CAL.rest_deg && parou_em < CAL.dump_deg, "parou entre o repouso e o despejo");
  doser_begin_meal(2, Origin::APP, CAL);
  roda();
  CHECK(doser_take_result(r) && r.confirmed == 2, "refeicao seguinte normal");
  CHECK(g_attach_jump == 0 && g_max_jump <= SERVO_STEP_DEG, "sem tranco ao retomar");

  printf("D10 calibracao: segura, anda devagar e estaciona\n");
  reset(100, true);
  CHECK(doser_servo_hold(70), "segurar com o doseador parado");
  espera(1000);
  CHECK(g_attached && g_angle == 70, "segura em 70");
  CHECK(doser_servo_hold(30), "ajuste");
  espera(1000);
  CHECK(g_attached && g_angle == 30, "segura em 30");
  CHECK(doser_servo_park(CAL.rest_deg), "estacionar no repouso");
  espera(1000);
  CHECK(!g_attached && g_angle == CAL.rest_deg, "chegou e soltou");
  CHECK(g_attach_jump == 0 && g_max_jump <= SERVO_STEP_DEG, "sem tranco na calibracao");
  CHECK(!doser_busy() && !doser_take_result(r), "nao e refeicao");

  printf("D11 no meio de uma refeicao, movimento manual e recusado\n");
  reset(100, true);
  doser_begin_meal(2, Origin::AGENDA, CAL);
  g_ms++;
  doser_update();
  CHECK(!doser_servo_hold(150) && !doser_servo_park(150), "recusado");
  CHECK(doser_phase() == DoserPhase::WARNING, "fase de refeicao");
  roda();
  CHECK(doser_take_result(r) && r.confirmed == 2 && g_angle == CAL.rest_deg, "refeicao intacta");

  printf("D12 o teste do aviso tem fase propria\n");
  doser_begin_warning_test();
  g_ms++;
  doser_update();
  CHECK(doser_phase() == DoserPhase::WARNING_TEST, "WARNING_TEST, nao refeicao");
  roda();

  printf("D13 teste do feixe: autoteste e conta o que passar a mao\n");
  reset(100, true);
  doser_begin_beam_test(5);
  espera(1000);
  CHECK(doser_phase() == DoserPhase::BEAM_TEST && g_led && g_blocked >= 0, "LED aceso e contando");
  g_count += 3;   // tres graos passados pelo feixe
  uint32_t dur_feixe = roda();
  CHECK(doser_beam_count() == 3 && doser_sensor_ok() && doser_sensor_tested(), "3 contados, sensor ok");
  CHECK(!g_led && g_blocked < 0 && !g_attached && g_m1 == 0 && g_m2 == 0, "so o feixe, e desligado no fim");
  CHECK(dur_feixe >= 3900 && dur_feixe <= 4100, "durou o pedido");
  CHECK(!doser_take_result(r), "nao e refeicao");

  printf("D14 teste do feixe com o sensor quebrado: para no autoteste\n");
  reset(100, false);
  doser_begin_beam_test(5);
  roda();
  CHECK(!doser_sensor_ok() && doser_beam_count() == 0 && !g_led, "reprovado, LED apagado");

  printf("D15 teste do feixe tem teto\n");
  reset(100, true);
  doser_begin_beam_test(600);
  CHECK(roda() <= (uint32_t)BEAM_TEST_MAX_S * 1000 + 200, "60 s no maximo");
  CHECK(!doser_take_result(r), "e nao vira refeicao pelo teto da refeicao");

  return resumo();
}
