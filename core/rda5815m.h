/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RDA5815M_H
#define RDA5815M_H

#include <stdint.h>
#include "it9300.h"

#define RDA5815M_I2C_ADDR 0x0c

int rda5815m_init(it9300 *bridge, uint8_t addr);
int rda5815m_tune(it9300 *bridge, uint8_t addr,
                  uint32_t frequency_khz, uint32_t symbol_rate_ksps,
                  int loop_through);

#endif
