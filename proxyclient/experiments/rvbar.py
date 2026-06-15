# SPDX-License-Identifier: MIT
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1 import asm

p.smp_start_secondaries()

code = u.malloc(0x4000)
c = asm.ARMAsm("""
    // enable timer interrupts
    msr CNTP_TVAL_EL0, x0
    mov x1, #1
    msr CNTP_CTL_EL0, x1

    isb
    wfi

    hvc #67
    b .
""", code)
iface.writemem(code, c.data)
p.dc_cvau(code, len(c.data))
p.ic_ivau(code, len(c.data))

freq = u.mrs(CNTFRQ_EL0)

ovrd = u.mrs(CYC_OVRD_EL1)
ovrd |= (3<<24)|(1 << 0)
u.msr(CYC_OVRD_EL1, ovrd)

p.call(code, round(freq / 100))
