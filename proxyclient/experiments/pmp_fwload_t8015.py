#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib, time
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1 import asm

if len(sys.argv) != 2:
    print(
        f"Usage: {sys.argv[0]} /path/to/PMP_firmware.bin\n" +
        "Firmware is in __DATA,fwFirmwareImage of Apple*PMPFirmware.kext"
        )
    exit(0)

f = open(sys.argv[1],"rb")

# remap a chunk of AP's physical memory AKF_REMAP_AP to
# iop's physical memory AKF_REMAP_IOP for size AKF_REMAP_SZ

AKF_REMAP_AP_LO  = 0x08
AKF_REMAP_AP_HI  = 0x10
AKF_REMAP_IOP_LO = 0x18
AKF_REMAP_IOP_HI = 0x20
AKF_REMAP_SZ_LO  = 0x28
AKF_REMAP_SZ_HI  = 0x30
AKF_REMAP_ENABLE = 0x38
AKF_REG_80C      = 0x80c

# Set PMP SRAM to be PMP's 0x0
p.pmgr_adt_power_enable("/arm-io/pmp")
PMP_AKF  = u.adt["/arm-io/pmp"].get_reg(0)[0]
PMP_SRAM = u.adt["/arm-io/pmp"].get_reg(1)[0]
PMP_CPU  = u.adt["/arm-io/pmp"].get_reg(2)[0]
PMP_SRAM_SIZE = u.adt["/arm-io/pmp"].get_reg(1)[1]


print(f"Writing FW to {hex(PMP_SRAM)}")
fw_buf = f.read()
iface.writemem(PMP_SRAM, fw_buf)

print(f"Configuring AKF")

p.write32(PMP_AKF + AKF_REMAP_IOP_LO, 0)
p.write32(PMP_AKF + AKF_REMAP_IOP_HI, 0)
p.write32(PMP_AKF + AKF_REMAP_SZ_LO, (PMP_SRAM_SIZE >> 0) & 0xffffffff)
p.write32(PMP_AKF + AKF_REMAP_SZ_HI, (PMP_SRAM_SIZE >> 32) & 0xffffffff)
p.write32(PMP_AKF + AKF_REMAP_AP_LO, (PMP_SRAM >> 0) & 0xffffffff)
p.write32(PMP_AKF + AKF_REMAP_AP_HI, (PMP_SRAM >> 32) & 0xffffffff)
p.write32(PMP_AKF + AKF_REMAP_ENABLE, 1)

# Not sure about what this does...
p.write32(PMP_AKF + AKF_REG_80C, 0)
p.write32(PMP_AKF + AKF_REG_80C, 0x2)

print(f"Start CPU")
p.write32(PMP_CPU, 1)

print(f"Now you can use StandardASC to handle PMP")
