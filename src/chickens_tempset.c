#include "cpu_regs.h"
#include "utils.h"

void init_common_tempset(void)
{
    // Prevent ordered loads from being dispatched from LSU until all prior loads have completed.
    // rdar://problem/34095873: AF2 ordering rules allow ARM device ordering violations
    reg_set(SYS_IMP_APL_EHID4, EHID4_FORCE_NS_ORD_LD_REQ_NO_IN_PIPE_ORD_LD);

    // rdar://problem/36595004: Poisoned younger load is not redirected by older load-acquire
    reg_set(SYS_IMP_APL_EHID3, EHID3_DISABLE_COLOR_OPTIONS);

    // rdar://problem/37949166: Disable the extension of prefetcher training pipe clock gating,
    // revert to default gating
    reg_set(SYS_IMP_APL_EHID10, EHID10_RCC_DISABLE_POWER_SAVE_PREFETCHER_CLOCK_OFF);
}

void init_t8020_tempset(int rev)
{
    UNUSED(rev);

    init_common_tempset();
}
