/* SPDX-License-Identifier: MIT */

#include "usb.h"
#include "adt.h"
#include "dart.h"
#include "i2c.h"
#include "iodev.h"
#include "malloc.h"
#include "pmgr.h"
#include "soc.h"
#include "string.h"
#include "tps6598x.h"
#include "types.h"
#include "usb_complex.h"
#include "usb_dwc2.h"
#include "usb_dwc3.h"
#include "usb_dwc3_regs.h"
#include "usb_types.h"
#include "utils.h"
#include "vsprintf.h"

struct usb_drd_regs {
    uintptr_t drd_regs;
    uintptr_t drd_regs_unk3;
    uintptr_t atc;
};

#if USB_IODEV_COUNT > 100
#error "USB_IODEV_COUNT is limited to 100 to prevent overflow in ADT path names"
#endif

#ifdef USE_DEBUG_USB
#define FIRST_USB_IODEV 1
#else
#define FIRST_USB_IODEV 0
#endif

// length of the format string is is used as buffer size
// limits the USB instance numbers to reasonable 2 digits
#define FMT_DART_PATH        "/arm-io/dart-usb%u"
#define FMT_DART_MAPPER_PATH "/arm-io/dart-usb%u/mapper-usb%u"
#define FMT_ATC_PATH         "/arm-io/atc-phy%u"
#define FMT_DRD_PATH         "/arm-io/usb-drd%u"
// HPM_PATH string is at most
// "/arm-io/i2cX" (12) + "/" + hpmBusManagerX (14) + "/" + "hpmX" (4) + '\0'
#define MAX_HPM_PATH_LEN 40

static tps6598x_irq_state_t tps6598x_irq_state[USB_IODEV_COUNT];
static bool usb_is_initialized = false;
usb_type_t usb_type = USB_TYPE_DWC3;

#define PIPEHANDLER_MUX_CTRL             0x0c
#define PIPEHANDLER_MUX_CTRL_USB3        0x08
#define PIPEHANDLER_MUX_CTRL_USB4_TUNNEL 0x11
#define PIPEHANDLER_MUX_CTRL_DUMMY       0x22

#define PIPEHANDLER_LOCK_REQ 0x10
#define PIPEHANDLER_LOCK_ACK 0x14
#define PIPEHANDLER_LOCK_EN  BIT(0)

#define PIPEHANDLER_AON_GEN                     0x1C
#define PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN BIT(4)
#define PIPEHANDLER_AON_GEN_DWC3_RESET_N        BIT(0)

#define PIPEHANDLER_NONSELECTED_OVERRIDE 0x20
#define PIPEHANDLER_NATIVE_RESET         BIT(12)
#define PIPEHANDLER_DUMMY_PHY_EN         BIT(15)
#define PIPEHANDLER_NATIVE_POWER_DOWN    GENMASK(3, 0)

static dart_dev_t *usb_dart_init(u32 idx)
{
    int mapper_offset;
    char path[sizeof(FMT_DART_MAPPER_PATH)];

    snprintf(path, sizeof(path), FMT_DART_MAPPER_PATH, idx, idx);
    mapper_offset = adt_path_offset(adt, path);
    if (mapper_offset < 0) {
        // Device not present
        return NULL;
    }

    u32 dart_idx;
    if (ADT_GETPROP(adt, mapper_offset, "reg", &dart_idx) < 0) {
        printf("usb: Error getting DART %s device index/\n", path);
        return NULL;
    }

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    return dart_init_adt(path, 1, dart_idx, false);
}

static int usb_drd_get_regs(u32 idx, struct usb_drd_regs *regs)
{
    int adt_drd_path[8];
    int adt_drd_offset;
    int adt_phy_path[8];
    int adt_phy_offset;
    char phy_path[sizeof(FMT_ATC_PATH)];
    char drd_path[sizeof(FMT_DRD_PATH)];

    snprintf(drd_path, sizeof(drd_path), FMT_DRD_PATH, idx);
    adt_drd_offset = adt_path_offset_trace(adt, drd_path, adt_drd_path);
    if (adt_drd_offset < 0) {
        // Nonexistent device
        return -1;
    }

    snprintf(phy_path, sizeof(phy_path), FMT_ATC_PATH, idx);
    adt_phy_offset = adt_path_offset_trace(adt, phy_path, adt_phy_path);
    if (adt_phy_offset < 0) {
        printf("usb: Error getting phy node %s\n", phy_path);
        return -1;
    }

    if (adt_get_reg(adt, adt_phy_path, "reg", 0, &regs->atc, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", phy_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 0, &regs->drd_regs, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", drd_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 3, &regs->drd_regs_unk3, NULL) < 0) {
        printf("usb: Error getting reg with index 3 for %s.\n", drd_path);
        return -1;
    }

    return 0;
}

int usb_phy_bringup(u32 idx)
{
    char path[24];

    if (idx >= USB_IODEV_COUNT)
        return -1;

    struct usb_drd_regs usb_regs;
    if (usb_drd_get_regs(idx, &usb_regs) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_ATC_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DRD_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    write32(usb_regs.atc + 0x08, 0x01c1000f);
    write32(usb_regs.atc + 0x04, 0x00000003);
    write32(usb_regs.atc + 0x04, 0x00000000);
    write32(usb_regs.atc + 0x1c, 0x008c0813);
    write32(usb_regs.atc + 0x00, 0x00000002);

    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_DUMMY);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_NONSELECTED_OVERRIDE, 0x9332);

    return 0;
}

dwc3_dev_t *usb_iodev_bringup(u32 idx)
{
    dart_dev_t *usb_dart = usb_dart_init(idx);
    if (!usb_dart)
        return NULL;

    struct usb_drd_regs usb_reg;
    if (usb_drd_get_regs(idx, &usb_reg) < 0)
        return NULL;

    return usb_dwc3_init(usb_reg.drd_regs, usb_dart);
}

#define USB_IODEV_WRAPPER(driver, name, pipe)                                                      \
    static ssize_t usb_##driver##_##name##_can_read(void *dev)                                     \
    {                                                                                              \
        return usb_##driver##_can_read(dev, pipe);                                                 \
    }                                                                                              \
                                                                                                   \
    static bool usb_##driver##_##name##_can_write(void *dev)                                       \
    {                                                                                              \
        return usb_##driver##_can_write(dev, pipe);                                                \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##driver##_##name##_read(void *dev, void *buf, size_t count)                \
    {                                                                                              \
        return usb_##driver##_read(dev, pipe, buf, count);                                         \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##driver##_##name##_write(void *dev, const void *buf, size_t count)         \
    {                                                                                              \
        return usb_##driver##_write(dev, pipe, buf, count);                                        \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##driver##_##name##_queue(void *dev, const void *buf, size_t count)         \
    {                                                                                              \
        return usb_##driver##_queue(dev, pipe, buf, count);                                        \
    }                                                                                              \
                                                                                                   \
    static void usb_##driver##_##name##_handle_events(void *dev)                                   \
    {                                                                                              \
        usb_##driver##_handle_events(dev);                                                         \
    }                                                                                              \
                                                                                                   \
    static void usb_##driver##_##name##_flush(void *dev)                                           \
    {                                                                                              \
        usb_##driver##_flush(dev, pipe);                                                           \
    }

USB_IODEV_WRAPPER(dwc2, 0, CDC_ACM_PIPE_0)
USB_IODEV_WRAPPER(dwc2, 1, CDC_ACM_PIPE_1)

USB_IODEV_WRAPPER(dwc3, 0, CDC_ACM_PIPE_0)
USB_IODEV_WRAPPER(dwc3, 1, CDC_ACM_PIPE_1)

#define USB_IODEV_OPS(driver, name, pipe)                                                          \
    {                                                                                              \
        .can_read = usb_##driver##_##name##_can_read,                                              \
        .can_write = usb_##driver##_##name##_can_write,                                            \
        .read = usb_##driver##_##name##_read,                                                      \
        .write = usb_##driver##_##name##_write,                                                    \
        .queue = usb_##driver##_##name##_queue,                                                    \
        .flush = usb_##driver##_##name##_flush,                                                    \
        .handle_events = usb_##driver##_##name##_handle_events,                                    \
    }

static struct iodev_ops iodev_usb_dwc2_ops = USB_IODEV_OPS(dwc2, 0, CDC_ACM_PIPE_0);
static struct iodev_ops iodev_usb_dwc2_sec_ops = USB_IODEV_OPS(dwc2, 1, CDC_ACM_PIPE_1);

static struct iodev_ops iodev_usb_dwc3_ops = USB_IODEV_OPS(dwc3, 0, CDC_ACM_PIPE_0);
static struct iodev_ops iodev_usb_dwc3_sec_ops = USB_IODEV_OPS(dwc3, 1, CDC_ACM_PIPE_1);

struct iodev iodev_usb_vuart = {
    .usage = 0,
    .lock = SPINLOCK_INIT,
};

static tps6598x_dev_t *hpm_init(i2c_dev_t *i2c, const char *hpm_path)
{
    tps6598x_dev_t *tps = tps6598x_init(hpm_path, i2c);
    if (!tps) {
        printf("usb: tps6598x_init failed for %s.\n", hpm_path);
        return NULL;
    }

    if (tps6598x_powerup(tps) < 0) {
        printf("usb: tps6598x_powerup failed for %s.\n", hpm_path);
        tps6598x_shutdown(tps);
        return NULL;
    }

    return tps;
}

void usb_spmi_init(void)
{
    for (int idx = 0; idx < USB_IODEV_COUNT; ++idx)
        usb_phy_bringup(idx); /* Fails on missing devices, just continue */

    usb_is_initialized = true;
}

static int usb_init_i2c(const char *i2c_path)
{
    char hpm_path[MAX_HPM_PATH_LEN];

    int node = adt_path_offset(adt, i2c_path);
    if (node < 0)
        return 0;

    node = adt_first_child_offset(adt, node);
    if (node < 0)
        return 0;

    if (!adt_is_compatible(adt, node, "usbc,manager"))
        return 0;

    const char *hpm_mngr_name = adt_get_name(adt, node);
    if (!hpm_mngr_name || strnlen(hpm_mngr_name, 16) >= 16)
        return 0;

    i2c_dev_t *i2c = i2c_init(i2c_path);
    if (!i2c) {
        printf("usb: i2c init failed for %s\n", i2c_path);
        return -1;
    }

    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        if (!name || memcmp(name, "hpm", 3) || name[4] != '\0')
            continue; // unexpected hpm node name
        u32 idx = name[3] - '0';
        if (idx >= USB_IODEV_COUNT)
            continue; // unexpected hpm index

        snprintf(hpm_path, sizeof(hpm_path), "%s/%s/%s", i2c_path, hpm_mngr_name, name);

        tps6598x_dev_t *tps = hpm_init(i2c, hpm_path);
        if (!tps) {
            printf("usb: failed to init %s\n", name);
            continue;
        }

        if (tps6598x_disable_irqs(tps, &tps6598x_irq_state[idx]))
            printf("usb: unable to disable IRQ masks for %s\n", name);

        tps6598x_shutdown(tps);
    }

    i2c_shutdown(i2c);

    return 0;
}

int usb_complex_init(struct usb_complex_config *config)
{
    usb_type = USB_TYPE_DWC2;

    // based on ADT's /arm-io/usb-complex usb_widget, we actually want to remap most stuff
    // here since Linux can't do 64-bit EHCI
    switch (config->type) {
        case USBCOMPLEX_S5L8960X:
            write32(config->USBComplexBase + USBX_EHCI0_REMAP_CTL_S5L8960X,
                    USBX_REMAP_TO_DRAM_BITS_S5L8960X);
            write32(config->USBComplexBase + USBX_EHCI1_REMAP_CTL_S5L8960X,
                    USBX_REMAP_TO_DRAM_BITS_S5L8960X);
            write32(config->USBComplexBase + USBX_USBDEV_REMAP_CTL_S5L8960X,
                    USBX_REMAP_TO_DRAM_BITS_S5L8960X);
            write32(config->USBComplexBase + USBX_OHCI0_REMAP_CTL_S5L8960X,
                    USBX_REMAP_TO_DRAM_BITS_S5L8960X);
            break;
        case USBCOMPLEX_T8011:
            write32(config->USBComplexBase + USBX_CTL_T8011, USBX_CTL_EN_T8011);
            write32(config->USBComplexBase + USBX_USB2DEV_REMAP_CTL_T8011,
                    USBX_REMAP_TO_DRAM_BITS_T8011);
            write32(config->USBComplexBase + USBX_EHCI_REMAP_CTL_T8011,
                    USBX_REMAP_TO_DRAM_BITS_T8011);
            break;
        case USBCOMPLEX_T8015:
            write32(config->USBComplexBase + USBX_CTL_T8011, USBX_CTL_EN_T8011);
            write32(config->USBComplexBase + USBX_EHCI0_REMAP_CTL_T8015,
                    USBX_REMAP_TO_DRAM_BITS_T8011);
            write32(config->USBComplexBase + USBX_OHCI0_REMAP_CTL_T8015,
                    USBX_REMAP_TO_DRAM_BITS_T8011);
            write32(config->USBComplexBase + USBX_EHCI1_REMAP_CTL_T8015,
                    USBX_REMAP_TO_DRAM_BITS_T8011);
            write32(config->USBComplexBase + USBX_USBDEV_REMAP_CTL_T8015,
                    USBX_REMAP_TO_DRAM_BITS_T8011);
            break;
        default:
            printf("usb: Unsupported complex type!\n");
            return -1;
    }

    write32(config->USB2Phy_Base + USBX_OTG_CFG0, config->cfg0_device);
    write32(config->USB2Phy_Base + USBX_OTG_CFG1, config->cfg1_device);

    set32(config->USB2Phy_Base + USBX_OTG_CTL, USBX_OTG_CTL_RESET);

    udelay(20);
    clear32(config->USB2Phy_Base + USBX_OTG_CTL, USBX_OTG_CTL_PWRDOWN | USBX_OTG_CTL_SIDDQ);
    udelay(20);
    clear32(config->USB2Phy_Base + USBX_OTG_CTL, USBX_OTG_CTL_RESET);
    udelay(20);
    clear32(config->USB2Phy_Base + USBX_OTG_SIG, USBX_OTG_SIG_VBUSDET_FORCE_EN);
    udelay(1500);

    dwc2_dev_t *opaque;
    struct iodev *usb_iodev;

    opaque = usb_dwc2_init(config->DWC2Base);
    if (!opaque)
        return -1;

    usb_iodev = memalign(SPINLOCK_ALIGN, sizeof(*usb_iodev));
    if (!usb_iodev)
        return -1;
    set32(config->USB2Phy_Base + USBX_OTG_SIG, USBX_OTG_SIG_VBUSDET_FORCE_EN);
    usb_iodev->ops = &iodev_usb_dwc2_ops;
    usb_iodev->opaque = opaque;
    usb_iodev->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
    spin_init(&usb_iodev->lock);

    iodev_register_device(IODEV_USB0, usb_iodev);
    printf("USB0: initialized at %p\n", opaque);

    usb_is_initialized = true;

    return 0;
}

int usb_complex_init_adt(void)
{
    int otgphyctrl_path[8], usbComplex_path[8];
    int usbComplex_offset, otgctl_offset;
    enum usb_complex_type type;
    u64 USBComplexBase, USB2Phy_Base = 0, DWC2Base;

    usbComplex_offset = adt_path_offset_trace(adt, "/arm-io/usb-complex", usbComplex_path);

    if (usbComplex_offset < 0)
        return -1;

    otgctl_offset = adt_path_offset_trace(adt, "/arm-io/otgphyctrl", otgphyctrl_path);
    if (otgctl_offset < 0) {
        printf("usb: No /arm-io/otgphyctrl node \n");
        return -1;
    }

    for (uint32_t i = 0, max = 2; i < max; ++i) {
        u64 ctlsize, ctlbase;
        if (adt_get_reg(adt, otgphyctrl_path, "reg", i, &ctlbase, &ctlsize) < 0) {
            printf("usb: failed to get /arm-io/otgphyctrl reg\n");
            return -1;
        }
        if (ctlsize == 0x20) {
            USB2Phy_Base = ctlbase;
            break;
        }
    }

    if (!USB2Phy_Base) {
        printf("usb: failed to parse /arm-io/otgphyctrl reg\n");
        return -1;
    }

    if (adt_get_reg(adt, usbComplex_path, "reg", 0, &USBComplexBase, NULL) < 0) {
        printf("usb: Error getting USBComplexBase Reg\n");
        return -1;
    }

    u32 cfg0, cfg1;
    if (ADT_GETPROP(adt, otgctl_offset, "cfg0-device", &cfg0) < 0) {
        printf("usb: Error getting CFG0 from otgctl \n");
        return -1;
    }
    if (ADT_GETPROP(adt, otgctl_offset, "cfg1-device", &cfg1) < 0) {
        printf("usb: Error getting CFG1 from otgctl \n");
        return -1;
    }

    // the usb-device on iPad Pro 2 is some USB3 device so derive dwc2 base from phy base instead
    DWC2Base = (USB2Phy_Base & ~0xfffULL) + 0x100000;

    if (adt_is_compatible(adt, usbComplex_offset, "usb-complex,s5l8960x")) {
        type = USBCOMPLEX_S5L8960X;
    } else if (adt_is_compatible(adt, usbComplex_offset, "usb-complex,t8015")) {
        type = USBCOMPLEX_T8015;
    }
    // This must be last because of the fallback compatible to usb-complex,t8011 on t8015
    else if (adt_is_compatible(adt, usbComplex_offset, "usb-complex,t8011")) {
        type = USBCOMPLEX_T8011;
    } else {
        printf("usb: unsupported USB complex type!\n");
        return -1;
    }

    if (pmgr_adt_power_enable_index("/arm-io/usb-complex", 0) < 0) {
        printf("usb: could not enable /arm-io/usb-complex power domain 0\n");
        return -1;
    }

    if (pmgr_adt_power_enable_index("/arm-io/usb-complex", 1) < 0) {
        printf("usb: could not enable /arm-io/usb-complex power domain 1\n");
        return -1;
    }

    struct usb_complex_config config = {
        .USBComplexBase = USBComplexBase,
        .USB2Phy_Base = USB2Phy_Base,
        .DWC2Base = DWC2Base,
        .cfg0_device = cfg0,
        .cfg1_device = cfg1,
        .type = type,
    };

    return usb_complex_init(&config);
}

void usb_init(void)
{
    if (usb_is_initialized)
        return;

    /*
     * M3/M4 models do not use i2c, but instead SPMI with a new controller.
     * We can get USB going for now by just bringing up the phys.
     */
    if (adt_path_offset(adt, "/arm-io/nub-spmi-a0/hpm0") > 0) {
        usb_spmi_init();
        return;
    }

    /*
     * A7-A11 uses a custom internal otg phy with the peripheral part
     * being dwc2, role switch seems custom.
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0) {
        usb_complex_init_adt();
        return;
    }

    if (adt_is_compatible(adt, 0, "J180dAP") && usb_init_i2c("/arm-io/i2c3") < 0)
        return;
    if (usb_init_i2c("/arm-io/i2c0") < 0)
        return;

    for (int idx = 0; idx < USB_IODEV_COUNT; ++idx)
        usb_phy_bringup(idx); /* Fails on missing devices, just continue */

    usb_is_initialized = true;
}

void usb_i2c_restore_irqs(const char *i2c_path, bool force)
{
    char hpm_path[MAX_HPM_PATH_LEN];

    int node = adt_path_offset(adt, i2c_path);
    if (node < 0)
        return;

    node = adt_first_child_offset(adt, node);
    if (node < 0)
        return;

    if (!adt_is_compatible(adt, node, "usbc,manager"))
        return;

    const char *hpm_mngr_name = adt_get_name(adt, node);
    if (!hpm_mngr_name || strnlen(hpm_mngr_name, 16) >= 16)
        return;

    i2c_dev_t *i2c = i2c_init(i2c_path);
    if (!i2c) {
        printf("usb: i2c init failed.\n");
        return;
    }

    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        if (!name || memcmp(name, "hpm", 3) || name[4] != '\0')
            continue; // unexpected hpm node name
        u32 idx = name[3] - '0';
        if (idx >= USB_IODEV_COUNT)
            continue; // unexpected hpm index

        if (iodev_get_usage(IODEV_USB0 + idx) && !force)
            continue;

        if (tps6598x_irq_state[idx].valid) {
            snprintf(hpm_path, sizeof(hpm_path), "%s/%s/%s", i2c_path, hpm_mngr_name, name);
            tps6598x_dev_t *tps = hpm_init(i2c, hpm_path);
            if (!tps)
                continue;

            if (tps6598x_restore_irqs(tps, &tps6598x_irq_state[idx]))
                printf("usb: unable to restore IRQ masks for %s\n", name);

            tps6598x_shutdown(tps);
        }
    }

    i2c_shutdown(i2c);
}

void usb_hpm_restore_irqs(bool force)
{
    /*
     * Do not try to restore irqs on M3/M4 which don't use i2c
     */
    if (adt_path_offset(adt, "/arm-io/nub-spmi-a0/hpm0") > 0)
        return;

    /*
     * Do not try to restore irqs on A7-A11 which don't use i2c
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0)
        return;

    if (adt_is_compatible(adt, 0, "J180dAP"))
        usb_i2c_restore_irqs("/arm-io/i2c3", force);
    usb_i2c_restore_irqs("/arm-io/i2c0", force);
}

void usb_iodev_init(void)
{
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0) {
        return; // already init in usb_init() since we do have only 1 usb port
    }
    for (int i = FIRST_USB_IODEV; i < USB_IODEV_COUNT; i++) {
        dwc3_dev_t *opaque;
        struct iodev *usb_iodev;

        opaque = usb_iodev_bringup(i);
        if (!opaque)
            continue;

        usb_iodev = memalign(SPINLOCK_ALIGN, sizeof(*usb_iodev));
        if (!usb_iodev)
            continue;

        usb_iodev->ops = &iodev_usb_dwc3_ops;
        usb_iodev->opaque = opaque;
        usb_iodev->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
        spin_init(&usb_iodev->lock);

        iodev_register_device(IODEV_USB0 + i, usb_iodev);
        printf("USB%d: initialized at %p\n", i, opaque);
    }
}

void usb_iodev_shutdown(void)
{
    for (int i = FIRST_USB_IODEV; i < USB_IODEV_COUNT; i++) {
        struct iodev *usb_iodev = iodev_unregister_device(IODEV_USB0 + i);
        if (!usb_iodev)
            continue;

        printf("USB%d: shutdown\n", i);
        if (usb_type == USB_TYPE_DWC2) {
            usb_dwc2_shutdown(usb_iodev->opaque);
            return;
        } else {
            usb_dwc3_shutdown(usb_iodev->opaque);
        }
        free(usb_iodev);
    }
}

void usb_iodev_vuart_setup(iodev_id_t iodev)
{
    if (iodev < IODEV_USB0 || iodev >= IODEV_USB0 + USB_IODEV_COUNT)
        return;

    iodev_usb_vuart.ops =
        usb_type == USB_TYPE_DWC2 ? &iodev_usb_dwc2_sec_ops : &iodev_usb_dwc3_sec_ops;
    iodev_usb_vuart.opaque = iodev_get_opaque(iodev);
}
