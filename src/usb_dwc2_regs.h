/* SPDX-License-Identifier: (GPL-2.0+ OR BSD-3-Clause) */
/*
 * Parts from linux commit 5bab5dc780c9ed0c69fc2f828015532acf4a7848
 * hw.h - DesignWare HS OTG Controller hardware definitions
 *
 * Copyright 2004-2013 Synopsys, Inc.
 */

#ifndef USB_DWC2_REGS_H
#define USB_DWC2_REGS_H

#include "types.h"

/* Constants */
#define DWC2_DSTS_CONNECTSPD 0x6
#define DWC2_DSTS_HIGHSPEED  0x0

#define DWC2_EP_TYPE_CONTROL 0
#define DWC2_EP_TYPE_ISOC    1
#define DWC2_EP_TYPE_BULK    2
#define DWC2_EP_TYPE_INTR    3

/* Global registers */
#define DWC2_GOTGCTL 0x000
#define DWC2_GOTGINT 0x004

#define DWC2_GAHBCFG              0x008
#define DWC2_GAHBCFG_HBSTLEN_MASK GENMASK(4, 1)
#define DWC2_GAHBCFG_HBSTLEN(x)   ((x & 0xf) << 1)
#define DWC2_GAHBCFG_DMA_EN       BIT(5)

#define DWC2_GUSBCFG           0x00c
#define DWC2_GUSBCFG_PHYIF16   BIT(3)
#define GUSBCFG_USBTRDTIM_MASK GENMASK(13, 10)

#define DWC2_GRSTCTL         0x010
#define DWC2_GRSTCTL_CSFTRST BIT(0)

#define DWC2_GINTSTS            0x014
#define DWC2_GINTSTS_GOUTNakEff BIT(7)
#define DWC2_GINTSTS_USBSuspend BIT(11)
#define DWC2_GINTSTS_USBRst     BIT(12)
#define DWC2_GINTSTS_ENUMDone   BIT(13)
#define DWC2_GINTSTS_IEPInt     BIT(18)
#define DWC2_GINTSTS_OEPInt     BIT(19)

#define DWC2_GINTMSK             (0x018)
#define DWC2_GINTMSK_OEPIntMsk   BIT(19)
#define DWC2_GINTMSK_IEPIntMsk   BIT(18)
#define DWC2_GINTSTS_ENUMDoneMsk BIT(13)
#define DWC2_GINTSTS_USBRstMsk   BIT(12)

#define DWC2_GRXSTSR   0x01c
#define DWC2_GRXSTSP   0x020
#define DWC2_GRXFSIZ   0x024
#define DWC2_GNPTXFSIZ 0x028
#define DWC2_GNPTXSTS  0x02c
#define DWC2_GI2CCTL   0x030
#define DWC2_GPVNDCTL  0x034
#define DWC2_GGPIO     0x038
#define DWC2_GUID      0x03c

#define DWC2_GSNPSID      0x040
#define DWC2_GSNPSID_MASK 0xffff0000

#define DWC2_GHWCFG1   0x044
#define DWC2_GHWCFG2   0x048
#define DWC2_GHWCFG3   0x04c
#define DWC2_GHWCFG4   0x050
#define DWC2_GLPMCFG   0x054
#define DWC2_GPWRDN    0x058
#define DWC2_GDFIFOCFG 0x05c

/* Attachment detection control register */
#define DWC2_ADPCTL (0x060)

/* Host registers */
#define DWC2_HPTXFSIZ   0x100
#define DWC2_DTXFSIZ(n) (0x104 + 0x4 * (n - 1))
#define DWC2_HPTXSIZ    0x400
#define DWC2_HPRT0      0x440

/* Device registers */
#define DWC2_DCFG            0x800
#define DCFG_NZ_STS_OUT_HSHK BIT(2)

#define DWC2_DCTL           0x804
#define DWC2_DCTL_SftDisCon BIT(1)
#define DWC2_DCTL_SGOUTNak  BIT(9)
#define DWC2_DCTL_CGOUTNak  BIT(10)

#define DWC2_DSTS 0x808

#define DWC2_DIEPMSK              0x810
#define DWC2_DIEPMSK_XferComplMsk BIT(0)
#define DWC2_DIEPMSK_AHBErrMsk    BIT(2)
#define DWC2_DIEPMSK_TimeOUTMsk   BIT(3)

#define DWC2_DOEPMSK              0x814
#define DWC2_DOEPMSK_XferComplMsk BIT(0)
#define DWC2_DOEPMSK_AHBErrMsk    BIT(2)
#define DWC2_DOEPMSK_SetUPMsk     BIT(3)

#define DWC2_DAINT    (0x818)
#define DWC2_DAINTMSK (0x81c)

#define DWC2_DIEPCTL(ep)        (0x900 + 0x20 * ep)
#define DWC2_DXEPCTLi_EnableEP  BIT(31)
#define DWC2_DXEPCTLi_DisableEP BIT(30)
#define DWC2_DXEPCTLi_SetD0Pid  BIT(28)
#define DWC2_DXEPCTL_SetNAK     BIT(27)
#define DWC2_DXEPCTL_ClearNAK   BIT(26)
#define DWC2_DXEPCTL_Stall      BIT(21)
#define DWC2_DXEPCTL_ActivateEP BIT(15)

#define DWC2_DIEPINT(ep)                (0x908 + 0x20 * ep)
#define DWC2_DIEPINT_InTokenTXFifoEmpty BIT(4)
#define DWC2_DIEPINT_TimeOUT            BIT(3)
#define DWC2_DIEPINT_AHBErr             BIT(2)
#define DWC2_DIEPINT_EPDisabled         BIT(1)
#define DWC2_DIEPINT_XferCompl          BIT(0)

#define DWC2_DIEPTSIZ(ep) (0x910 + 0x20 * ep)
#define DWC2_DIEPDMA(ep)  (0x914 + 0x20 * ep)
#define DWC2_DTXFSTS(ep)  (0x918 + 0x20 * ep)
#define DWC2_DIEPDMAB(ep) (0x91c + 0x20 * ep)

#define DWC2_DOEPCTL(ep)            (0xb00 + 0x20 * ep)
#define DWC2_DOEPINT(ep)            (0xb08 + 0x20 * ep)
#define DWC2_DOEPINT_STUP_PKT_RCVD  BIT(15)
#define DWC2_DOEPINT_STS_PHASE_RCVD BIT(5)
#define DWC2_DOEPINT_SETUP          BIT(3)
#define DWC2_DOEPINT_AHBErr         BIT(2)
#define DWC2_DOEPINT_EPDisabled     BIT(1)
#define DWC2_DOEPINT_XFER_COMPL     BIT(0)

#define DWC2_DOEPTSIZ(ep) (0xb10 + 0x20 * ep)
#define DWC2_DOEPDMA(ep)  (0xb14 + 0x20 * ep)
#define DWC2_DOEPDMAB(ep) (0xb1c + 0x20 * ep)
#endif
