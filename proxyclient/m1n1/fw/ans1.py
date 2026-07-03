# SPDX-License-Identifier: MIT
import struct

from ..utils import *
from m1n1.utils import *
from m1n1.setup import *
from m1n1 import asm

from .akf import StandardAKF
from .akf.base import *

# trimmed request appears to be SUCCESS so must zero beforehand
# there also seem no way to distinguished between reading
# unwritten aux storage and errors

CMD_BUFFER_PER_TAG = 16320
NUM_TAGS = 16
CMDBUF_SIZE = NUM_TAGS * CMD_BUFFER_PER_TAG
NUM_NSID = 11

MAX_LBA_PER_CMD = (CMD_BUFFER_PER_TAG - 0x30) // 4

USED_TAG = 15

ASP_CMD_OP         = 0x0
ASP_CMD_LBA_OFF    = 0x4
ASP_CMD_NUM_LBA    = 0x8
ASP_CMD_OUT_BUFFER = 0x30

ASP_CMD_OP_Identify         = 0x0
ASP_CMD_OP_Read_UserArea    = 0x10
ASP_CMD_OP_Write_UserArea   = 0x11
"""
This format the whole NAND. "Format_Util" from syslog.
Don't run it! Syscfg is not recoverable with DFU restore.
This will also reset all auxiliary storages to "unwritten"
"""
ASP_CMD_OP_UNK12 = 0x12 # crashes with no args
ASP_CMD_OP_UNK13 = 0x13 # no error
ASP_CMD_OP_UNK14 = 0x14 # no error
ASP_CMD_OP_Format_Util      = 0x15
"""
Syslog claims this one is "Format_Neuralize", but considering that after
completing this command ANS1 will intentionally crash, this is probably
meant to be "Neutralize".

After sending this command and rebooting, NAND IO don't work (times
out) and the FORMAT_UTIL command must be ran before it works again,
so this command probably leaves NAND uninitialized after wiping
"""
ASP_CMD_OP_Format_UserArea  = 0x16
# 0x17 Not Exist
ASP_CMD_OP_Format_Clear     = 0x18
ASP_CMD_OP_Write_Unlock     = 0x19
ASP_CMD_OP_UNK1A            = 0x1a # no error
ASP_CMD_OP_UNK1B            = 0x1b # no error
ASP_CMD_OP_UNK1C            = 0x1c # no error

ASP_CMD_OP_READ_LLB         = 0x30
ASP_CMD_OP_READ_FW          = 0x31
ASP_CMD_OP_READ_UTILDM      = 0x32
ASP_CMD_OP_READ_DM          = 0x33
ASP_CMD_OP_READ_CTRLBITS    = 0x34
ASP_CMD_OP_READ_EFFACE      = 0x35
ASP_CMD_OP_READ_NVRAM       = 0x36
ASP_CMD_OP_READ_SYSCFG      = 0x37
ASP_CMD_OP_READ_PANICLOG    = 0x38

ASP_CMD_OP_WRITE_LLB        = 0x40
ASP_CMD_OP_WRITE_FW         = 0x41
ASP_CMD_OP_WRITE_UTILDM     = 0x42
ASP_CMD_OP_WRITE_DM         = 0x43
ASP_CMD_OP_WRITE_CTRLBITS   = 0x44
ASP_CMD_OP_WRITE_EFFACE     = 0x45
ASP_CMD_OP_WRITE_NVRAM      = 0x46
ASP_CMD_OP_WRITE_SYSCFG     = 0x47
ASP_CMD_OP_WRITE_PANICLOG   = 0x48

# 0x50-0x52 require writable, crash if write-protected 
# after running 0x50: "SysCfg not found" untethered crash
ASP_CMD_OP_UNK50            = 0x50
ASP_CMD_OP_UNK51            = 0x51
ASP_CMD_OP_UNK52            = 0x52 # Hangs with no args

# Command 0x72-0x79 updates the command buffer, except 0x74 (all with no args)
ASP_CMD_OP_UNK72            = 0x72
ASP_CMD_OP_UNK73            = 0x73
ASP_CMD_OP_UNK74            = 0x74
# Update command buffer with dates: "20141220"
ASP_CMD_OP_UNK75            = 0x75
ASP_CMD_OP_UNK76            = 0x76
ASP_CMD_OP_UNK77            = 0x77
ASP_CMD_OP_UNK78            = 0x78 # cmdbuf: "DP0" x4
ASP_CMD_OP_PPN_FW_Version   = 0x79

ASP_CMD_OP_UNK7A            = 0x7a # require writable

ASP_CMD_OP_UNK7B            = 0x7b
ASP_CMD_OP_UNK7C            = 0x7b
ASP_CMD_OP_UNK7D            = 0x7d
ASP_CMD_OP_UNK7E            = 0x7e

# "assert failed:segIdx: 1281" when used with no args
ASP_CMD_OP_UNK7F            = 0x7f

ASP_CMD_OP_POWER_CONFIG     = 0x80
ASP_CMD_OP_UNK81            = 0x81 # return 0xa
ASP_CMD_OP_UNK82            = 0x82 # return 0xa

# "invalid cell type for TLC device: 0" probably missing some values 
ASP_CMD_OP_UNK90            = 0x90
# require writable, then "GEB panic" if write enabled
ASP_CMD_OP_UNK91            = 0x91

ASP_CMD_OP_UNKA0            = 0xa0
ASP_CMD_OP_UNKA1            = 0xa1 # hangs

ASP_CMD_OP_Identify_Chip_Id = 0
ASP_CMD_OP_Identify_Mfg_Id  = 1

ASP_CMD_Bus_Stride = 0x10

read_ops = (0, 0x10, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38)
write_ops = (0, 0x11, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48)

code = u.malloc(0x1000)

assert ((CMD_BUFFER_PER_TAG * NUM_TAGS) & 0x3f) == 0
assert (CMD_BUFFER_PER_TAG & 0x3f) == 0 # align requirement
assert CMD_BUFFER_PER_TAG > 0x30
assert CMDBUF_SIZE < 262144
assert USED_TAG >= 0 and USED_TAG < NUM_TAGS

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


"""
example layout with 2 buses
bus also called "Channel" in some places
chipid_bus0
chipid_bus1
mfg_bus0
mfg_bus1
"""
def identify_bus_item_offset(u, cmd, bus, item):
    b = u.proxy.read32(cmd + 0x38) // ASP_CMD_Bus_Stride
    return 0x60 + ((item * b + bus) * 6)

class ANS_Message(Register64):
    EP = 63, 56
    TYPE = 3, 0

class ANS_SetBase(ANS_Message):
    BASE = 55, 16
    SZ_17_6 = 15, 4
    TYPE = 3, 0, Constant(0)

class ANS_SetTag(ANS_Message):
    SIZE_17_6 = 35, 24
    OFF_17_6 = 23, 12
    TAG = 7, 4
    TYPE = 3, 0, Constant(1)

class ANS_SQ_DB(ANS_Message):
    BASE = 55, 16
    OP = 19, 12, Constant(0xff)
    TAG = 7, 4
    TYPE = 3, 0, Constant(3)

class ANS_Reply(ANS_Message):
    EP = 63, 56
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
        self.send(ANS_SetTag(SIZE_17_6=CMD_BUFFER_PER_TAG>>6, OFF_17_6=(CMD_BUFFER_PER_TAG*tag)>>6, TAG=tag, TYPE=1))
        self.akf.work()
        self.mon.poll()

    def send_cmd(self, tag, io):
        self.akf.u.proxy.call(util.dma_wmb)
        self.in_progress = True
        self.send(ANS_SQ_DB(OP=0xff, TAG=tag, TYPE=3))
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
        
        if num > MAX_LBA_PER_CMD:
            print(f"Error: Max {MAX_LBA_PER_CMD} lba at once")
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

    def asp_send_op(self, op):
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, op | 8 << 16 | USED_TAG << 8)
        self.send_cmd(USED_TAG, True)

    def asp_write(self, nsid, lba, num, bfr):
        if not self.base:
            print("IO not initialized yet")
            return
        
        if nsid >= NUM_NSID:
            print("Invalid NSID!")
            return
        
        if num > MAX_LBA_PER_CMD:
            print(f"Error: Max {MAX_LBA_PER_CMD} lba at once")
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

    def dump_info(self):
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8 | ASP_CMD_OP_PPN_FW_Version)
        self.send_cmd(USED_TAG, True)

        bus_info = []

        num_bus = self.akf.u.proxy.read32(cmd + 0x30) // ASP_CMD_Bus_Stride

        for b in range(0, num_bus):
            ppn_fw = iface.readmem(cmd + 0x34, 0x10).decode('utf-8')
            bus_info.append([ppn_fw])

        # IDENTIFY
        cmd = self.cmdbuf_for_tag(USED_TAG)
        self.akf.u.proxy.write32(cmd + ASP_CMD_OP, USED_TAG << 8)
        self.send_cmd(USED_TAG, True)

        lba = self.akf.u.proxy.read32(cmd + 0x30)
        lba_sz = self.akf.u.proxy.read32(cmd + 0x34)
        raw_lba = self.akf.u.proxy.read32(cmd + 0x48)

        for b in range(0, num_bus):
            c = iface.readmem(cmd + identify_bus_item_offset(u, cmd, b, ASP_CMD_OP_Identify_Chip_Id), 6)
            m = iface.readmem(cmd + identify_bus_item_offset(u, cmd, b, ASP_CMD_OP_Identify_Mfg_Id), 6)
            bus_info[b].append(c)
            bus_info[b].append(m)


        print(f"[ansep] {lba} LBAs, sector size {lba_sz}, total {lba * lba_sz} bytes")
        print(f"[ansep] {raw_lba} Raw LBAs")

        for b in range(0, num_bus):
            print(f"[ansep] ANC Channel {b}:")
            print(f"[ansep]\tChip ID: {bus_info[b][1].hex(sep=' ')}")
            print(f"[ansep]\tManufacturer ID: {bus_info[b][2].hex(sep=' ')}")
            print(f"[ansep]\tPPN FW version: {bus_info[b][0]}")


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
        self.base = self.akf.u.proxy.memalign(0x1000, CMDBUF_SIZE)
        self.akf.u.proxy.memset32(self.base, 0, CMDBUF_SIZE)
        self.in_progress = True
        print(f"Base: {self.base:#x}")
        print(f"Tag {USED_TAG} command buffer: {self.base + USED_TAG * CMD_BUFFER_PER_TAG:#x}")
        self.send(ANS_SetBase(BASE=self.base, SZ_17_6=CMDBUF_SIZE >> 6, TYPE=0))
        while self.in_progress:
            self.akf.work()
            self.mon.poll()
        for i in range(0, NUM_TAGS):
            self.tag_setup(i)
        self.mon.poll()
        if (self.verbose >= 1):
            self.dump_info()
        if (u.adt["/chosen"].chip_id != 0x8960):
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
        0x5: ANSEndpoint,
        0x6: ANSEndpoint,
        0x20: ANSEndpoint,
    }
