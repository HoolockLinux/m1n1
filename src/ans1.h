/* SPDX-License-Identifier: MIT */
#ifndef ANS1_H
#define ANS1_H

#include "types.h"
#include "utils.h"

bool ans1_read_main_storage(u64 lba, void *buffer);
void ans1_shutdown(void);
bool ans1_init(void);

#endif
