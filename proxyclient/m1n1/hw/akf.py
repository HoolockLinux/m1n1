# SPDX-License-Identifier: MIT
from ..utils import *
import time
import struct

class R_MBOX_CTRL(Register32):
    EMPTY = 17
    FULL  = 16
    ENABLE = 0

class R_CPU_CONTROL(Register32):
    RUN    = 4

class AKF_Message(Register64):
    EP = 63, 56

# A7-A8
class AKF_V1_Regs(RegMap):
    REMAP_PHYS_LO = 0x8,  Register32
    REMAP_PHYS_HI = 0xc,  Register32
    REMAP_IOVA_LO = 0x10, Register32
    REMAP_IOVA_HI = 0x14, Register32
    REMAP_SIZE_LO = 0x18, Register32
    REMAP_SIZE_HI = 0x1c, Register32
    ENDIANNESS  = 0x20, Register32
    CPU_CONTROL   = 0x28, R_CPU_CONTROL
    UNK_80C       = 0x80C, Register32

    INBOX_CTRL  = 0x1008, R_MBOX_CTRL
    OUTBOX_CTRL = 0x1020, R_MBOX_CTRL
    INBOX       = 0x1010, AKF_Message
    OUTBOX      = 0x1038, AKF_Message

# A9-A10
class AKF_V2_Regs(RegMap):
    REMAP_PHYS_LO = 0x8,  Register32
    REMAP_PHYS_HI = 0xc,  Register32
    REMAP_IOVA_LO = 0x10, Register32
    REMAP_IOVA_HI = 0x14, Register32
    REMAP_SIZE_LO = 0x18, Register32
    ENDIANNESS  = 0x20, Register32
    REMAP_SIZE_HI = 0x1c, R_CPU_CONTROL
    CPU_CONTROL   = 0x28, Register32
    UNK_80C       = 0x80C

    INBOX_CTRL  = 0x4008, R_MBOX_CTRL
    OUTBOX_CTRL = 0x4020, R_MBOX_CTRL
    INBOX       = 0x4010, AKF_Message
    OUTBOX      = 0x4038, AKF_Message

class AKF:
    def __init__(self, u, adt_path):
        self.chip_id = u.adt["/chosen"].chip_id
        self.adt_path = adt_path
        self.u = u
        # only support preloaded akf proc now
        if self.chip_id in (0x8960, 0x7000, 0x7001):
            self.akf = AKF_V1_Regs(u, u.adt[adt_path].get_reg(0)[0])
        else:
            self.akf = AKF_V2_Regs(u, u.adt[adt_path].get_reg(0)[0])

        self.verbose = 3
        self.akf.INBOX_CTRL.val = 1
        self.akf.OUTBOX_CTRL.val = 1
        self.configured = False
        self.epmap = {}

    def adt_configure(self):
        nub = None

        for child in self.u.adt[self.adt_path]:
            if child.name.startswith("iop-"):
                nub = child

        if hasattr(nub, "segment_ranges"):
            (text_phys, text_iova, _, text_size, data_phys, data_iova, _, data_size) = struct.unpack('<QQQI4xQQQI4x', nub.segment_ranges)
            self.akf.REMAP_IOVA_LO.val = 0
            self.akf.REMAP_IOVA_HI.val = 0
            self.akf.REMAP_PHYS_LO.val = text_phys & 0xffffffff
            self.akf.REMAP_PHYS_HI.val = text_phys >> 32
            self.akf.REMAP_SIZE_LO.val = text_size + data_size
            self.akf.REMAP_SIZE_HI.val = 0
        else:
            self.akf.REMAP_IOVA_LO.val = 0
            self.akf.REMAP_IOVA_HI.val = 0
            self.akf.REMAP_PHYS_LO.val = nub.region_base & 0xffffffff
            self.akf.REMAP_PHYS_HI.val = nub.region_base >> 32
            self.akf.REMAP_SIZE_LO.val = nub.region_size
            self.akf.REMAP_SIZE_HI.val = 0
            
        self.akf.ENDIANNESS.val = 1
        self.akf.UNK_80C.val = 0
        self.configured = True

    def recv(self):
        if self.akf.OUTBOX_CTRL.reg.EMPTY:
            return None, None

        msg = self.akf.OUTBOX.val
        msg_format = AKF_Message(msg)
        if self.verbose >= 3:
            print(f"< {msg_format.EP:02x}:{msg:#x}")
        return msg

    def send(self, msg):
        self.akf.INBOX.val = msg

        if self.verbose >= 3:
            if isinstance(msg, Register):
                print(f"> {msg.EP:02x}:{msg._value:#x}")
            else:
                msg_format = AKF_Message(msg)
                print(f"> {msg_format.EP:02x}:{msg:#x}")

        while self.akf.INBOX_CTRL.reg.FULL:
            pass

    def is_running(self):
        return self.akf.CPU_CONTROL.reg.RUN

    def boot(self):
        if not self.configured:
            self.adt_configure()
        self.akf.CPU_CONTROL.set(RUN=1)

    def shutdown(self):
        self.akf.CPU_CONTROL.set(RUN=0)

    def add_ep(self, idx, ep):
        self.epmap[idx] = ep
        setattr(self, ep.SHORT, ep)

    def has_messages(self):
        return not self.akf.OUTBOX_CTRL.reg.EMPTY

    def work_pending(self):
        while self.has_messages():
            self.work()

    def work(self):
        if self.akf.OUTBOX_CTRL.reg.EMPTY:
            return True

        msg = self.recv()
        msg_format = AKF_Message(msg)

        handled = False

        ep = self.epmap.get(msg_format.EP, None)
        if ep:
            handled = ep.handle_msg(msg)

        if not handled:
            print(f"unknown message: {msg:#16x} / {msg_format.EP}")

        return handled

    def work_forever(self):
        while self.work():
            pass

    def work_for(self, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.work()
