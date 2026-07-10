#include "adt.h"
#include "types.h"
#include "utils.h"

/* New style with segment-ranges; iOS 12 */
static bool akf_fw_get_region_new(int fw_node, u64 *phys, u64 *iova, u64 *size)
{
    const struct adt_segment_ranges *seg;
    u32 segments_len;

    seg = adt_getprop(adt, fw_node, "segment-ranges", &segments_len);
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
static bool akf_fw_get_region_old(int fw_node, u64 *phys, u64 *iova, u64 *size)
{
    if (ADT_GETPROP(adt, fw_node, "region-base", phys) < 0) {
        printf("akf: Could not get region-base property\n");
        return false;
    }

    if (ADT_GETPROP(adt, fw_node, "region-size", size) < 0) {
        printf("akf: Could not get region-size property\n");
        return false;
    }

    *iova = 0;

    if (*size == 0)
        return false;

    return true;
}

bool akf_fw_get_region(int fw_node, u64 *phys, u64 *iova, u64 *size)
{
    int pre_loaded;
    if (ADT_GETPROP(adt, fw_node, "pre-loaded", &pre_loaded) < 0) {
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
    if (!akf_fw_get_region_new(fw_node, phys, iova, size))
        if (!akf_fw_get_region_old(fw_node, phys, iova, size))
            return false;

    return true;
}
