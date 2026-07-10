#ifndef AKF_FW_H
#define AKF_FW_H

#include "types.h"

bool akf_fw_get_region(int fw_node, u64 *phys, u64 *iova, u64 *size);

#endif
