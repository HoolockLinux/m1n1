/* SPDX-License-Identifier: MIT */

#include "adt.h"
#include "akf.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

#define AKF_REMAP_PHYS_LO 0x8
#define AKF_REMAP_PHYS_HI 0xc
#define AKF_REMAP_IOVA_LO 0x10
#define AKF_REMAP_IOVA_HI 0x14
#define AKF_REMAP_SIZE_LO 0x18
#define AKF_REMAP_SIZE_HI 0x1c
#define AKF_ENDIANNESS    0x20

#define AKF_CPU_CONTROL       0x28
#define AKF_CPU_CONTROL_START BIT(4)

#define AKF_UNK_80C 0x80c

#define AKF_V1_OFF 0x1000
#define AKF_V2_OFF 0x4000

#define AKF_MBOX_CONTROL_ENABLE BIT(0)
#define AKF_MBOX_CONTROL_FULL   BIT(16)
#define AKF_MBOX_CONTROL_EMPTY  BIT(17)

#define AKF_MBOX_SET 0x0
#define AKF_MBOX_CLR 0x4

#define AKF_MBOX_A2I_CONTROL 0x8
#define AKF_MBOX_A2I_SEND0   0x10
#define AKF_MBOX_A2I_SEND1   0x14
#define AKF_MBOX_A2I_RECV0   0x18
#define AKF_MBOX_A2I_RECV1   0x1c
#define AKF_MBOX_I2A_CONTROL 0x20

#define AKF_MBOX_I2A_SEND0 0x30
#define AKF_MBOX_I2A_SEND1 0x34
#define AKF_MBOX_I2A_RECV0 0x38
#define AKF_MBOX_I2A_RECV1 0x3c

struct akf_dev {
    uintptr_t base;
    uintptr_t cpu_base;
    int iop_node;
};

akf_dev_t *akf_init(const char *path)
{
    int akf_path[8];
    int node = adt_path_offset_trace(path, akf_path);
    if (node < 0) {
        printf("akf: Error getting akf node %s\n", path);
        return NULL;
    }

    u64 base;
    if (adt_get_reg(akf_path, "reg", 0, &base, NULL) < 0) {
        printf("akf: Error getting akf %s base address.\n", path);
        return NULL;
    }

    akf_dev_t *akf = calloc(1, sizeof(*akf));
    if (!akf)
        return NULL;

    akf->cpu_base = base;
    if (adt_is_compatible(node, "iop,s5l8960x")) {
        akf->base = base + AKF_V1_OFF;
    } else if (adt_is_compatible(node, "iop,s8000")) {
        akf->base = base + AKF_V2_OFF;
    } else {
        printf("akf: Unsupported compatible\n");
        return NULL;
    }

    akf->iop_node = node;

    /* Enable mailbox (required on A7) */
    set32(akf->base + AKF_MBOX_A2I_CONTROL, AKF_MBOX_CONTROL_ENABLE);
    set32(akf->base + AKF_MBOX_I2A_CONTROL, AKF_MBOX_CONTROL_ENABLE);

    return akf;
}

/* New style with segment-ranges; iOS 12 */
static bool akf_get_fw_region_new(int fw_node, u64 *phys, u64 *iova, u64 *size)
{
    const struct adt_segment_ranges *seg;
    u32 segments_len;

    seg = adt_getprop(fw_node, "segment-ranges", &segments_len);
    if (!seg) {
        printf("akf: No segment-ranges property\n");
        return false;
    }

    unsigned int count = segments_len / sizeof(*seg);
    if (!count) {
        printf("akf: segment-ranges too short\n");
        return false;
    }

    u64 region_phys = seg->phys;
    u64 region_iova = seg->iova;
    u64 region_phys_end = seg->phys + seg->size;

    for (unsigned int i = 0; i < count; i++) {
        if (seg[i].iova < region_iova)
            region_iova = seg[i].iova;

        if (seg[i].phys < region_phys)
            region_phys = seg[i].phys;

        if ((seg[i].phys + seg[i].size) > region_phys_end)
            region_phys_end = seg[i].phys + seg[i].size;
    }

    *phys = region_phys;
    *iova = region_iova;
    *size = region_phys_end - region_phys;

    return true;
}

/* Old style with region-base and region-size; iOS 10 */
static bool akf_get_fw_region_old(int fw_node, u64 *phys, u64 *iova, u64 *size)
{
    if (ADT_GETPROP(fw_node, "region-base", phys) < 0) {
        printf("akf: Could not get region-base property\n");
        return false;
    }

    if (ADT_GETPROP(fw_node, "region-size", size) < 0) {
        printf("akf: Could not get region-size property\n");
        return false;
    }

    *iova = 0;

    if (*size == 0)
        return false;

    return true;
}

bool akf_map_preloaded_fw(akf_dev_t *akf)
{
    int fw_node = adt_first_child_offset(akf->iop_node);
    if (!fw_node) {
        printf("akf: IOP Firmware node not found\n");
        return false;
    }

    int pre_loaded;
    if (ADT_GETPROP(fw_node, "pre-loaded", &pre_loaded) < 0) {
        printf("akf: Could not get pre-loaded property\n");
        return false;
    }

    if (!pre_loaded) {
        printf("akf: IOP firmware not pre-loaded.\n");
        return false;
    }

    /*
     * Always check the new one first, because in ADTs of certain versions
     * it will be segment-ranges, but the unfilled region-base and region-size
     * still exists
     */
    u64 phys, iova, size;

    if (!akf_get_fw_region_new(fw_node, &phys, &iova, &size))
        if (!akf_get_fw_region_old(fw_node, &phys, &iova, &size))
            return false;

    write32(akf->cpu_base + AKF_REMAP_IOVA_LO, iova & 0xffffffff);
    write32(akf->cpu_base + AKF_REMAP_IOVA_HI, iova >> 32);
    write32(akf->cpu_base + AKF_REMAP_PHYS_LO, phys & 0xffffffff);
    write32(akf->cpu_base + AKF_REMAP_PHYS_HI, phys >> 32);
    write32(akf->cpu_base + AKF_REMAP_SIZE_LO, size & 0xffffffff);
    write32(akf->cpu_base + AKF_REMAP_SIZE_HI, size >> 32);
    /* 0 = BE, 1 = LE */
    write32(akf->cpu_base + AKF_ENDIANNESS, 1);
    write32(akf->cpu_base + AKF_UNK_80C, 0);

    return true;
}

void akf_cpu_start(akf_dev_t *akf)
{
    set32(akf->cpu_base + AKF_CPU_CONTROL, AKF_CPU_CONTROL_START);
}

void akf_cpu_stop(akf_dev_t *akf)
{
    clear32(akf->cpu_base + AKF_CPU_CONTROL, AKF_CPU_CONTROL_START);
}

bool akf_cpu_running(akf_dev_t *akf)
{
    return !!(read32(akf->cpu_base + AKF_CPU_CONTROL) & AKF_CPU_CONTROL_START);
}

void akf_free(akf_dev_t *akf)
{
    free(akf);
}

int akf_get_iop_node(akf_dev_t *akf)
{
    return akf->iop_node;
}

bool akf_can_recv(akf_dev_t *akf)
{
    return !(read32(akf->base + AKF_MBOX_I2A_CONTROL) & AKF_MBOX_CONTROL_EMPTY);
}

bool akf_can_send(akf_dev_t *akf)
{
    return !(read32(akf->base + AKF_MBOX_A2I_CONTROL) & AKF_MBOX_CONTROL_FULL);
}

bool akf_send(akf_dev_t *akf, u64 msg)
{
    if (poll32(akf->base + AKF_MBOX_A2I_CONTROL, AKF_MBOX_CONTROL_FULL, 0, 200000)) {
        printf("akf: A2I mailbox full for 200ms. Is the akf stuck?");
        return false;
    }

    dma_wmb();
    write64(akf->base + AKF_MBOX_A2I_SEND0, msg);

    // printf("sent msg: 0x%lx\n", msg);
    return true;
}

bool akf_recv(akf_dev_t *akf, u64 *msg)
{
    if (!akf_can_recv(akf))
        return false;

    *msg = read64(akf->base + AKF_MBOX_I2A_RECV0);
    dma_rmb();

    // printf("received msg: 0x%lx\n", *msg);
    return true;
}

bool akf_recv_timeout(akf_dev_t *akf, u64 *msg, u32 delay_usec)
{
    u64 timeout = timeout_calculate(delay_usec);
    while (!timeout_expired(timeout)) {
        if (akf_recv(akf, msg))
            return true;
    }
    return false;
}
