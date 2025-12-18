/* SPDX-License-Identifier: MIT */

#ifndef AKF_H
#define AKF_H

#include "types.h"

typedef struct akf_dev akf_dev_t;

akf_dev_t *akf_init(const char *path);
void akf_free(akf_dev_t *akf);

int akf_get_iop_node(akf_dev_t *akf);

void akf_cpu_start(akf_dev_t *akf);
void akf_cpu_stop(akf_dev_t *akf);
bool akf_cpu_running(akf_dev_t *akf);

bool akf_can_recv(akf_dev_t *akf);
bool akf_can_send(akf_dev_t *akf);

bool akf_send(akf_dev_t *akf, u64 msg);
bool akf_recv(akf_dev_t *akf, u64 *msg);
bool akf_recv_timeout(akf_dev_t *akf, u64 *msg, u32 delay_usec);

#endif
