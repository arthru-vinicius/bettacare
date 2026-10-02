#pragma once
#include <stdio.h>

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, what)                                                   \
  do {                                                                      \
    if (cond) { g_ok++; }                                                   \
    else { g_fail++; printf("  FALHOU: %s  (linha %d)\n", what, __LINE__); } \
  } while (0)

static int resumo() {
  printf("\n%d verificacoes ok, %d falharam\n", g_ok, g_fail);
  return g_fail ? 1 : 0;
}
