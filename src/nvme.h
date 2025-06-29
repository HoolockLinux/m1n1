/* SPDX-License-Identifier: MIT */

#ifndef NVME_H
#define NVME_H

#include "types.h"

bool nvme_init(void);
void nvme_shutdown(void);

bool nvme_flush(u32 nsid);
bool nvme_read(u32 nsid, u64 lba, void *buffer);

enum nvme_type {
    /* Non-standard 128-byte IOSQEs */
    NVME_TYPE_T8015,

    /* NVMMU and linear submission queues */
    NVME_TYPE_T8103,
};

#endif
