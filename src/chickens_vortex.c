#include "cpu_regs.h"
#include "utils.h"

void init_common_vortex(void)
{
    // rdar://problem/34435356: segfaults due to IEX clock-gating
    reg_set(SYS_IMP_APL_HID1, HID1_RCC_FORCE_ALL_IEX_L3_CLKS_ON);

    // Prevent ordered loads from being dispatched from LSU until all prior loads have completed.
    // rdar://problem/34095873: AF2 ordering rules allow ARM device ordering violations"
    reg_set(SYS_IMP_APL_HID4, HID4_FORCE_NS_ORD_LD_REQ_NO_OLDER_LD);

    // rdar://problem/38482968: [Cyprus Tunable] Poisoned cache line crossing younger load is not
    // redirected by older load-barrier
    reg_set(SYS_IMP_APL_HID3, HID3_DISABLE_COLOR_OPTIONS);

    // rdar://problem/41056604: disable faster launches of uncacheable unaligned stores to
    // workaround load/load ordering violation
    reg_set(SYS_IMP_APL_HID11, HID11_DISABLE_X64_NT_LAUNCH_OPTION);
}

void init_t8020_vortex(int rev)
{
    init_common_vortex();

    // Should be applied to all Aruba variants, but only Cyprus variants B0 and later
    // rdar://problem/36716477: data corruption due to incorrect branch predictor resolution
    if (rev >= 0x10)
        reg_set(SYS_IMP_APL_HID1, HID1_ENABLE_BR_KILL_LIMIT);
}
