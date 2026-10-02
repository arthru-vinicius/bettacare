#pragma once
typedef int gpio_num_t;
/** Nível lido no pino — o teste muda para simular o fio com e sem módulo. */
inline int g_gpio_level = 1;
inline int gpio_pullup_en(gpio_num_t) { return 0; }
inline int gpio_pullup_dis(gpio_num_t) { return 0; }
inline int gpio_pulldown_en(gpio_num_t) { return 0; }
inline int gpio_get_level(gpio_num_t) { return g_gpio_level; }
