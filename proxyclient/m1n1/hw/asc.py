# SPDX-License-Identifier: MIT
from ..utils import *
import time

class R_MBOX_CTRL(Register32):
    FIFOCNT = 23, 20
    OVERFLOW = 18
    EMPTY = 17
    FULL  = 16
    RPTR = 15, 12
    WPTR = 11, 8
    ENABLE = 0

class R_CPU_CONTROL(Register32):
    RUN    = 4

class R_CPU_STATUS(Register32):
    IDLE            = 5
    FIQ_NOT_PEND    = 3 # guess
    IRQ_NOT_PEND    = 2 # guess
    STOPPED         = 1
    RUNNING         = 0

class R_INBOX1(Register64):
    EP      = 7, 0

class R_OUTBOX1(Register64):
    OUTCNT  = 56, 52
    INCNT   = 51, 48
    OUTPTR  = 47, 44
    INPTR   = 43, 40
    EP      = 7, 0


class T8015ASCRegs(RegMap):
    INBOX_CTRL  = 0x8108, R_MBOX_CTRL
    OUTBOX_CTRL = 0x810c, R_MBOX_CTRL
    INBOX0      = 0x8800, Register64
    INBOX1      = 0x8808, R_INBOX1
    OUTBOX0     = 0x8830, Register64
    OUTBOX1     = 0x8838, R_OUTBOX1

class ASCRegs(RegMap):
    CPU_CONTROL = 0x0044, R_CPU_CONTROL
    CPU_STATUS  = 0x0048, R_CPU_STATUS

    INBOX_CTRL  = 0x8110, R_MBOX_CTRL
    OUTBOX_CTRL = 0x8114, R_MBOX_CTRL
    INBOX0      = 0x8800, Register64
    INBOX1      = 0x8808, R_INBOX1
    OUTBOX0     = 0x8830, Register64
    OUTBOX1     = 0x8838, R_OUTBOX1

class ASC:
    def __init__(self, u, asc_base):
        self.chip_id = u.adt["/chosen"].chip_id

        self.u = u
        self.p = u.proxy
        self.iface = u.iface
        if self.chip_id == 0x8012:
            self.asc = T8015ASCRegs(u, asc_base)
            if asc_base == 0x203000000: # ans2
                self.start_reg = 0x204d20044
                self.start_val = 0x10
            elif asc_base == 0x20da00000: # sep
                self.start_reg = 0 # will be treated as always on
            elif asc_base == 0x212800000: # smc
                self.start_reg = 0x212000100
                self.start_val = 1
            elif asc_base == 0x210800000: # aop
                self.start_reg = 0x210000200
                self.start_val = 1
            else:
                raise ValueError("Unsupported ASC address")
        elif self.chip_id == 0x8015:
            self.asc = T8015ASCRegs(u, asc_base)
            if asc_base == 0x257000000: # ans2
                self.start_reg = 0x259d20044
                self.start_val = 0x10
            elif asc_base == 0x243000000: # sep
                self.start_reg = 0 # will be treated as always on
            elif asc_base == 0x236800000: # smc
                self.start_reg = 0x236000100
                self.start_val = 1
            elif asc_base == 0x234800000: # aop
                self.start_reg = 0x234000200
                self.start_val = 1
            elif asc_base == 0x232300000: # pmp
                self.start_reg = 0x232400000
                self.start_val = 1
            else:
                raise ValueError("Unsupported ASC address")
        else:
            self.asc = ASCRegs(u, asc_base)
        self.verbose = 0
        self.epmap = {}

    def recv(self):
        if self.asc.OUTBOX_CTRL.reg.EMPTY:
            return None, None

        msg0 = self.asc.OUTBOX0.val
        msg1 = R_INBOX1(self.asc.OUTBOX1.val)
        if self.verbose >= 3:
            print(f"< {msg1.EP:02x}:{msg0:#x}")
        return msg0, msg1

    def send(self, msg0, msg1):
        self.asc.INBOX0.val = msg0
        self.asc.INBOX1.val = msg1

        if self.verbose >= 3:
            if isinstance(msg0, Register):
                print(f"> {msg1.EP:02x}:{msg0}")
            else:
                print(f"> {msg1.EP:02x}:{msg0:#x}")

        while self.asc.INBOX_CTRL.reg.FULL:
            pass

    def is_running(self):
        if self.chip_id not in (0x8012, 0x8015):
            return not self.asc.CPU_STATUS.reg.STOPPED
        elif not self.start_reg:
            return True
        else:
            return not not (self.p.read32(self.start_reg) & self.start_val)

    def boot(self):
        if self.chip_id not in (0x8012, 0x8015):
            self.asc.CPU_CONTROL.set(RUN=1)
        elif not self.start_reg:
            return
        else:
            self.p.set32(self.start_reg, self.start_val)

    def shutdown(self):
        if self.chip_id not in (0x8012, 0x8015):
            self.asc.CPU_CONTROL.set(RUN=0)
        elif not self.start_reg:
            return
        else:
            self.p.clear32(self.start_reg, self.start_val)

    def add_ep(self, idx, ep):
        self.epmap[idx] = ep
        setattr(self, ep.SHORT, ep)

    def has_messages(self):
        return not self.asc.OUTBOX_CTRL.reg.EMPTY

    def work_pending(self):
        while self.has_messages():
            self.work()

    def work(self):
        if self.asc.OUTBOX_CTRL.reg.EMPTY:
            return True

        msg0, msg1 = self.recv()

        handled = False

        ep = self.epmap.get(msg1.EP, None)
        if ep:
            handled = ep.handle_msg(msg0, msg1)

        if not handled:
            print(f"unknown message: {msg0:#16x} / {msg1}")

        return handled

    def work_forever(self):
        while self.work():
            pass

    def work_for(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.work()
