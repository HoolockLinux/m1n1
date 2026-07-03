# SPDX-License-Identifier: MIT
import struct

from ..utils import *
from m1n1.utils import *
from m1n1.setup import *
from m1n1 import asm

from .akf import StandardAKF
from .akf.base import *

CMD_BUFFER_PER_TAG = 2240
NUM_NSID = 11

# Tag 8 works sometimes but the firmware would eventually crash, so there
# is probably a tag array inside the firmware for tags that is 8 long
NUM_TAGS = 8

USED_TAG = 7

ASP_CMD_OP         = 0x0
ASP_CMD_LBA_OFF    = 0x4
ASP_CMD_NUM_LBA    = 0x8
ASP_CMD_OUT_BUFFER = 0x30
ASP_CMD_MAX_BUFS   = 512

ASP_CMD_OP_Format_Util      = 0x15
ASP_CMD_OP_Format_LBA       = 0x16
ASP_CMD_OP_Format_Neuralize = 0x18

read_ops  = (0, 0x10, 0x36, 0x31, 0x30, 0x35, 0x37, 0x38, 0x32, 0x33, 0x34)
write_ops = (0, 0x11, 0x46, 0x41, 0x40, 0x45, 0x47, 0x48, 0x42, 0x43, 0x44)

code = u.malloc(0x1000)

util = asm.ARMAsm("""
dma_rmb:
    dmb oshld
    ret
dma_wmb:
    dmb oshst
    ret
""", code)

iface.writemem(code, util.data)
p.dc_cvau(code, len(util.data))
p.ic_ivau(code, len(util.data))

class ANS_Message(Register64):
    EP = 63, 56, Constant(0x20)

class ANS_SetBase(ANS_Message):
    BASE = 55, 16
    UNK = 15, 4, Constant(0x118)
    IO = 1
    CMD = 0

class ANS_Cmd(ANS_Message):
    UNK = 31, 24
    ARG_2 = 19, 16 # (TAG * 2) & 0xf
    ARG_3 = 15, 12 # (TAG * 3) & 0xf
    TAG = 11, 4
    IO = 1 # this probably means "use command buffer"
    CMD = 0

class ANS_IO_Cmd(ANS_Cmd):
    IO = 1

class ANS_Reply(ANS_Message):
    EP = 63, 56, Constant(0x20)
    STATUS = 15, 12
    TAG = 11, 4
    TYPE = 3, 0

class ANSEndpoint(AKFBaseEndpoint):
    BASE_MESSAGE = ANS_Message
    SHORT = "ansep"

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.mon = RegMonitor(u, ascii=True, bufsize=0x8000000)
        self.base = None
        self.in_progress = False
        self.verbose = 1

    # this has to do with setting up the command buffer
    def tag_setup(self, tag):
        # ANS_Admin_Cmd(ARG_2=arg2, ARG_3=arg3, TAG=tag, UNK=0x23, IO=0, CMD=1)
        self.send(ANS_Message((0x23000001 | tag << 4 | 0x20 << 56) + (0x23000 * tag)))
        self.akf.work()
        self.mon.poll()

    def send_cmd(self, tag, io):
        self.akf.u.proxy.call(util.dma_wmb)
        self.in_progress = True
        self.send(ANS_Cmd(ARG_2=0xf, ARG_3=0xf, TAG=tag, IO=io, CMD=1))
        while self.in_progress:
            self.akf.work()
        self.akf.u.proxy.call(util.dma_rmb)
        return self.base
    
    def cmdbuf_for_tag(self, tag=0):
        buf = self.base + CMD_BUFFER_PER_TAG * tag
        self.akf.u.proxy.memset32(buf, 0, CMD_BUFFER_PER_TAG)
        return buf

    def asp_read(self, nsid, lba, num, bfr):
        if not self.base:
            print("IO not initialized yet")
            return
        
        if nsid >= NUM_NSID:
            print("Invalid NSID!")
            return
        
        if num > 512:
            print("Error: Max 512 lba at once")
            return

        # this seems to be what determines which NSID gets read
        # message's nsid might just be the offset into command buffer (?)
        if not read_ops[nsid]:
            print(f"NSID {nsid} has unknown read OP")
            return
        
        if ((bfr & 0xffffff00000000fff) != 0):
            print("Buffer not 0x1000 aligned")
            return

        self.mon.poll()

        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, read_ops[nsid] | 8 << 16 | USED_TAG << 8)
        self.akf.u.proxy.write32(cmd + ASP_CMD_LBA_OFF, lba)
        self.akf.u.proxy.write32(cmd + ASP_CMD_NUM_LBA, num) # num buffers in out_buffer

        for i in range(0, num):
            self.akf.u.proxy.write32(cmd + ASP_CMD_OUT_BUFFER + i * 4, (bfr >> 12) + i)

        self.send_cmd(USED_TAG, True)

    def asp_write(self, nsid, lba, num, bfr):
        if not self.base:
            print("IO not initialized yet")
            return
        
        if nsid >= NUM_NSID:
            print("Invalid NSID!")
            return
        
        if num > 512:
            print("Error: Max 512 lba at once")
            return

        # this seems to be what determines which NSID gets read
        # message's nsid might just be the offset into command buffer (?)
        if not read_ops[nsid]:
            print(f"NSID {nsid} has unknown read OP")
            return
        
        if ((bfr & 0xffffff00000000fff) != 0):
            print("Buffer not 0x1000 aligned")
            return

        self.mon.poll()

        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, write_ops[nsid] | 8 << 16 | USED_TAG << 8)
        self.akf.u.proxy.write32(cmd + ASP_CMD_LBA_OFF, lba)
        self.akf.u.proxy.write32(cmd + ASP_CMD_NUM_LBA, num) # num buffers in out_buffer

        for i in range(0, num):
            self.akf.u.proxy.write32(cmd + ASP_CMD_OUT_BUFFER + i * 4, (bfr >> 12) + i)

        self.send_cmd(USED_TAG, True)

    def set_writable(self):
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, 0x19 | 8 << 16 | USED_TAG << 8)

        self.send_cmd(USED_TAG, True)

    def start(self):
        pass

    def identify(self):
        # IDENTIFY
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8)
        self.send_cmd(USED_TAG, True)

        lba = self.akf.u.proxy.read32(cmd + 0x30)
        lba_sz = self.akf.u.proxy.read32(cmd + 0x34)

        print(f"[ansep] {lba} LBAs, sector size {lba_sz}, total {lba * lba_sz} bytes")

    def cmd_init(self):
        # These are done by iboot after identification before first disk I/O
        # This updates the command buffer, so maybe another identification
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8 | 0x72) # 'r'
        self.send_cmd(USED_TAG, True)

        # These are possibly power management
        # maybe tunables?
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8 | 0x80)
        self.akf.u.proxy.write32(cmd + 0x38, 0x2a)
        self.akf.u.proxy.write32(cmd + 0x44, 0x400000)
        self.akf.u.proxy.write32(cmd + 0x48, 0x400000)
        self.send_cmd(USED_TAG, True)

        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8 | 0x80)
        self.akf.u.proxy.write32(cmd + 0x38, 0x13)
        self.akf.u.proxy.write32(cmd + 0x3c, 0x3)
        self.akf.u.proxy.write32(cmd + 0x44, 0x2)
        self.akf.u.proxy.write32(cmd + 0x48, 0x2)
        self.akf.u.proxy.write32(cmd + 0x4c, 0x4)
        self.akf.u.proxy.write32(cmd + 0x50, 0x2)
        self.akf.u.proxy.write32(cmd + 0x54, 0x4)
        self.akf.u.proxy.write32(cmd + 0x58, 0x4)
        self.akf.u.proxy.write32(cmd + 0x5c, 0x2)
        self.akf.u.proxy.write32(cmd + 0x60, 0x2)
        self.send_cmd(USED_TAG, True)

        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8 | 0x80)
        self.akf.u.proxy.write32(cmd + 0x38, 0x25)
        self.akf.u.proxy.write32(cmd + 0x3c, 0x3)
        self.akf.u.proxy.write32(cmd + 0x44, 0x1)
        self.akf.u.proxy.write32(cmd + 0x48, 0x1)
        self.akf.u.proxy.write32(cmd + 0x4c, 0x1)
        self.akf.u.proxy.write32(cmd + 0x50, 0x1)
        self.akf.u.proxy.write32(cmd + 0x54, 0x1)
        self.akf.u.proxy.write32(cmd + 0x58, 0x1)
        self.akf.u.proxy.write32(cmd + 0x5c, 0x1)
        self.akf.u.proxy.write32(cmd + 0x60, 0x1)
        self.send_cmd(USED_TAG, True)

        # "setting asp to high power mode"
        # this updates the command buffer
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8 | 0x80)
        self.akf.u.proxy.write32(cmd + 0x38, 0x26)
        self.akf.u.proxy.write32(cmd + 0x44, 0x1)

        self.send_cmd(USED_TAG, True)

    def start_io(self):
        # this is used by the IOP so we use proxy function here
        self.base = self.akf.u.proxy.memalign(0x1000, CMD_BUFFER_PER_TAG * NUM_TAGS)
        self.akf.u.proxy.memset32(self.base, 0, CMD_BUFFER_PER_TAG * NUM_TAGS)   
        self.in_progress = True
        self.send(ANS_SetBase(BASE=self.base, UNK=0x118, IO=0, CMD=0))
        while self.in_progress:
            self.akf.work()
            self.mon.poll()
        for i in range(0, NUM_TAGS):
            self.tag_setup(i)
        self.mon.poll()
        if (self.verbose >= 1):
            self.identify()
        self.cmd_init()


    def stop(self):
        if self.base:
            self.akf.u.proxy.free(self.base)

    def handle_msg(self, msg):
        msg_f = ANS_Reply(msg)
        # 2 == complete, 4 == ???
        if self.akf.verbose >= 3:
            print(f"Tag: {msg_f.TAG} return status {msg_f.STATUS:#x}, type: {msg_f.TYPE:#x}")
        if (msg_f.TYPE == 2):
            self.in_progress = False
        if (msg_f.TYPE not in (2, 4)):
            print("Received Unknown ANS Message")
            return False
        return True

class ANSClient(StandardAKF):
    ENDPOINTS = {
        0x20: ANSEndpoint,
    }
