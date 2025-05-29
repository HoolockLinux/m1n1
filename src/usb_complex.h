/* SPDX-License-Identifier: MIT */
#ifndef USB_COMPLEX_H
#define USB_COMPLEX_H

#include "types.h"

// a few bits in a few registers are the same as atc, but still very different from atc overall

/* Constants */
/* Some controllers only accept 32-bit addresses, and PHY can remap it */
// For DRAM 0x800000000-0x8ffffffff
#define USBX_REMAP_VAL 0x8

/* USB complex registers (Bus?) */
/* S5L8960X registers */
#define USBX_USBDEV_REMAP_CTL_S5L8960X 0x1c
#define USBX_EHCI0_REMAP_CTL_S5L8960X  0x3c
#define USBX_OHCI0_REMAP_CTL_S5L8960X  0x5c
#define USBX_EHCI1_REMAP_CTL_S5L8960X  0x7c

#define USBX_REMAP_EN_S5L8960X       BIT(8)
#define USBX_REMAP_VAL_MASK_S5L8960X GENMASK(3, 0)

#define USBX_REMAP_TO_DRAM_BITS_S5L8960X                                                           \
    (USBX_REMAP_EN_S5L8960X | FIELD_PREP(USBX_REMAP_VAL_MASK_S5L8960X, USBX_REMAP_VAL))

/* T8011/T8015 registers */
#define USBX_CTL_T8011    0x0
#define USBX_CTL_EN_T8011 BIT(0)

#define USBX_REMAP_EN1_T8011 BIT(24)
#define USBX_REMAP_EN2_T8011 BIT(25)

#define USBX_REMAP_VAL_MASK1_T8011 GENMASK(3, 0)
#define USBX_REMAP_VAL_MASK2_T8011 GENMASK(7, 4)

#define USBX_REMAP_TO_DRAM_BITS_T8011                                                              \
    (FIELD_PREP(USBX_REMAP_VAL_MASK1_T8011, USBX_REMAP_VAL) |                                      \
     FIELD_PREP(USBX_REMAP_VAL_MASK2_T8011, USBX_REMAP_VAL) | USBX_REMAP_EN1_T8011 |               \
     USBX_REMAP_EN2_T8011)

/* T8011 registers */
#define USBX_USB3DEV_REMAP_CTL_T8011 0x18
#define USBX_USB2DEV_REMAP_CTL_T8011 0x24
#define USBX_EHCI_REMAP_CTL_T8011    0x74
#define USBX_XHCI_REMAP_CTL_T8011    0x84

/* T8015 registers */
#define USBX_EHCI0_REMAP_CTL_T8015  0x18
#define USBX_OHCI0_REMAP_CTL_T8015  0x28
#define USBX_EHCI1_REMAP_CTL_T8015  0x38
#define USBX_USBDEV_REMAP_CTL_T8015 0x48

/* custom OTG registers (PHY?) */

#define USBX_OTG_CTL       0x00
#define USBX_OTG_CTL_RESET BIT(0)

/* Some sort of power down, should be cleared on startup */
#define USBX_OTG_CTL_PWRDOWN BIT(2)
#define USBX_OTG_CTL_SIDDQ   BIT(3)

/* All UNK bits are found to be set in m1n1*/
/*
 * Cable connected does not include cables that are disconnected on the other side
 * Entry with USB-A adapter connected = 0x1c003
 * Entry with cable connected = 0xd103
 * Entry without cable = 0xd003
 * USB Proxy Active = 0x1c103
 * USB-A adapter connected after proxy has been active = 0x1c003
 * Cable disconnected after proxy has been active = 0xd003
 */
#define USBX_OTG_SIG 0x04

// Always set
#define USBX_OTG_UNK0 BIT(0)

#define USBX_OTG_SIG_VBUSDET_FORCE_EN BIT(1)
#define USBX_OTG_SIG_CABLE_CONNECTED  BIT(8)

/* Set on entry and when disconnected */
#define USBX_OTG_SIG_UNK12 BIT(12)

// Always set
#define USBX_OTG_UNK14 BIT(14)
#define USBX_OTG_UNK15 BIT(15)

// Set when USB is active (not on entry)
#define USBX_OTG_UNK16 BIT(16)

#define USBX_OTG_CFG0 0x08
#define USBX_OTG_CFG1 0x0c

enum usb_complex_type {
    USBCOMPLEX_S5L8960X,
    USBCOMPLEX_T8011,
    USBCOMPLEX_T8015,
};

struct usb_complex_config {
    u64 USBComplexBase;
    u64 USB2Phy_Base;
    u64 DWC2Base;
    u32 cfg0_device;
    u32 cfg1_device;
    enum usb_complex_type type;
};

#endif
