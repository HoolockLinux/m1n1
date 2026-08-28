# SPDX-License-Identifier: MIT
from m1n1.utils import *
from m1n1.setup import *
import struct

# this script only deals with the mailbox hardware

class R_IRQ_CONTROL(Register32):
    A2I_EMPTY = 0
    A2I_EMPTY_ADJ = 1
    A2I_NOT_EMPTY = 4
    A2I_NOT_EMPTY_ADJ = 5
    I2A_EMPTY = 8
    I2A_EMPTY_ADJ = 9
    I2A_NOT_EMPTY = 12
    I2A_NOT_EMPTY_ADJ = 13

class R_MBOX_CTRL(Register32):
    EMPTY = 17
    FULL  = 16
    ENABLE = 0

class R_CPU_CONTROL(Register32):
    RUN    = 4

class AKF_V1_Regs(RegMap):
    REMAP_PHYS_LO = 0x8,  Register32
    REMAP_PHYS_HI = 0xc,  Register32
    REMAP_IOVA_LO = 0x10, Register32
    REMAP_IOVA_HI = 0x14, Register32
    REMAP_SIZE_LO = 0x18, Register32
    REMAP_SIZE_HI = 0x1c, Register32
    ENDIANNESS    = 0x20, Register32
    CPU_CONTROL   = 0x28, R_CPU_CONTROL

    IRQ_MASK    = 0x1000, R_IRQ_CONTROL
    IRQ_EN      = 0x1004, R_IRQ_CONTROL
    A2I_CTRL    = 0x1008, R_MBOX_CTRL
    I2A_CTRL    = 0x1020, R_MBOX_CTRL
    A2I_SEND    = 0x1010, Register64
    A2I_RECV    = 0x1018, Register64
    I2A_SEND    = 0x1030, Register64
    I2A_RECV    = 0x1038, Register64

p.pmgr_adt_power_enable("/arm-io/ans")

akf = AKF_V1_Regs(u, u.adt["/arm-io/ans"].get_reg(0)[0])

MBOX_IRQS = u.adt["/arm-io/ans"].interrupts
AIC = u.adt["/arm-io/aic"].get_reg(0)[0]

def unmask_irq(irq):
	print(f"IRQ {irq} unmasked")
	irq_off = 4 * (irq >> 5)
	irq_bit = 1 << (irq & 0x1f)
	p.write32(AIC + 0x4180 + irq_off, irq_bit)

def mask_irq(irq):
	print(f"IRQ {irq} masked")
	irq_off = 4 * (irq >> 5)
	irq_bit = 1 << (irq & 0x1f)
	p.write32(AIC + 0x4100 + irq_off, irq_bit)

for irq in MBOX_IRQS:
    mask_irq(irq)


IRQ_A2I_NOT_EMPTY = MBOX_IRQS[0]
IRQ_A2I_EMPTY = MBOX_IRQS[1] # works
IRQ_I2A_NOT_EMPTY = MBOX_IRQS[2] # works
IRQ_I2A_EMPTY = MBOX_IRQS[3]

# now try emulate linux behaviour

print("Trying to Emulate Linux Behavior")

print("== Probe == ")

print("Enable Power Domain")
p.pmgr_adt_power_enable("/arm-io/ans")

print("FIFO enable at mailbox level")

akf.A2I_CTRL.val = 1
akf.I2A_CTRL.val = 1

print("== RTKit Init ==")

print(f"akf.I2A_CTRL={akf.I2A_CTRL}")

akf.IRQ_EN.val = R_IRQ_CONTROL(A2I_EMPTY=1, I2A_NOT_EMPTY=1)
print(f"AIC unmask IRQ_I2A_NOT_EMPTY {IRQ_I2A_NOT_EMPTY}")
unmask_irq(IRQ_I2A_NOT_EMPTY)

# AIC unmask will cause the not empty IRQ to be fired as the mailbox was
# not empty some point in the pass while it is unmask at the mailbox level
p.udelay(200000)
assert (akf.I2A_CTRL.val & 0x20000) == 0x20000
# Linux will handle this situation
akf.IRQ_MASK.val = R_IRQ_CONTROL(I2A_NOT_EMPTY=1)


print("== Send ==")

print("Mailbox unmask A2I_EMPTY")
akf.IRQ_EN.val = R_IRQ_CONTROL(A2I_EMPTY=1)

print(f"AIC Unmask IRQ_A2I_EMPTY={IRQ_A2I_EMPTY}")
unmask_irq(IRQ_A2I_EMPTY)

# linux would wait for interrupt for fire
# wait a bit and make sure interrupt has fired
p.udelay(200000)
mask_irq(IRQ_A2I_EMPTY)

print("Sending")

# send and emulate ASP reading it
akf.A2I_SEND = 0x4141414141414141
assert akf.A2I_RECV.val == 0x4141414141414141

print("Sent")

print("== Receive ==")

assert (akf.I2A_CTRL.val & 0x20000) == 0x20000
# seem that writing to IRQ_MASK would mask the irq on AKF mailbox
akf.IRQ_EN.val = R_IRQ_CONTROL(I2A_NOT_EMPTY=1)
akf.I2A_SEND.val = 0x4141414141414141
p.udelay(200000)
assert akf.I2A_RECV.val == 0x4141414141414141
unmask_irq(IRQ_I2A_NOT_EMPTY)

print("test some more")
# This is a mask, not ack, writing the mask here would make the irq not
# fire even though the fifo is empty
akf.IRQ_MASK.val = R_IRQ_CONTROL(A2I_EMPTY=1)
unmask_irq(IRQ_A2I_EMPTY)

print("Shut down properly")

akf.IRQ_MASK.val = R_IRQ_CONTROL(A2I_EMPTY=1, A2I_NOT_EMPTY=1,
                                  I2A_EMPTY=1, I2A_NOT_EMPTY=1)
akf.A2I_CTRL.val = 0
akf.I2A_CTRL.val = 0
for irq in MBOX_IRQS:
    mask_irq(irq)
p.pmgr_adt_power_disable("/arm-io/ans")
