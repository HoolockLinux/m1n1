/* SPDX-License-Identifier: MIT */

#ifndef SMC_H
#define SMC_H

#include "asc.h"
#include "rtkit.h"
#include "types.h"

typedef struct smc_dev smc_dev_t;

int smc_write_u32(smc_dev_t *smc, u32 key, u32 value);

smc_dev_t *smc_init(void);
void smc_power_on(void);
void smc_shutdown(smc_dev_t *smc);

int smc_poll(smc_dev_t *smc);
void smc_start_smi(smc_dev_t *smc, void (*smi_rx)(u64 msg));
void smc_send_smi(smc_dev_t *smc, u64 msg);

#endif
