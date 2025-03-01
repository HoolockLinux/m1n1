# SPDX-License-Identifier: MIT

import struct

from enum import IntEnum
from ..utils import *
from ..malloc import Heap

__all__ = ["DART8960Regs", "DART8960"]

class R_ERROR(Register32):
    FLAG = 31
    STREAM = 27, 24
    CODE = 23, 0
    NO_DAPF_MATCH = 11
    WRITE = 10
    SUBPAGE_PROT = 7
    PTE_READ_FAULT = 6
    READ_FAULT = 4
    WRITE_FAULT = 3
    NO_PTE = 2
    NO_PMD = 1
    NO_TTBR = 0

class R_STREAM_COMMAND(Register32):
    STREAM_0 = 8
    STREAM_1 = 9
    STREAM_2 = 10
    STREAM_3 = 11

    BUSY = 3
    INVALIDATE = 1

class R_TCR_STREAM(Register8):
    TRANSLATE_ENABLE = 7

class R_TCR(Register32):
    TCR_0 = 31, 24
    TCR_1 = 23, 16
    TCR_2 = 15, 8
    TCR_3 = 7, 0

class R_TTBR(Register32):
    VALID = 31
    ADDR = 30, 0

class R_REMAP(Register32):
    MAP3 = 31, 24
    MAP2 = 23, 16
    MAP1 = 15, 8
    MAP0 = 7, 0

class PTE_S5L8960X(Register64):
    OFFSET = 35, 12
    WPROTECT = 7
    VALID = 1, 0

class DART8960Regs(RegMap):
    STREAM_COMMAND  = 0x0, R_STREAM_COMMAND
    ERROR           = 0x10, R_ERROR
    ERROR_AXI_REQ0  = 0x14, Register32
    ERROR_AXI_REQ1  = 0x18, Register32
    DIAG_CONFIG     = 0x20, Register32
    UNK24           = 0x24, Register32
    ERROR_ADDR_LO   = 0x1c, Register32
    UNK2C           = 0x2c, Register32
    FETCH_CONFIG    = 0x30, Register32
    PERF_CONFIG     = 0x78, Register32

    TCR             = 0x0c, R_TCR
    TTBR            = (irange(0x40, 4, 16), range(0, 16, 4)), R_TTBR

PTE_TYPES = {
    "dart,s5l8960x": PTE_S5L8960X,
    "dart,t7000": PTE_S5L8960X,
    "dart,s8000": PTE_S5L8960X,
    "dart,t8010": PTE_S5L8960X,
}

class DART8960(Reloadable):
    PAGE_BITS = 12
    PAGE_SIZE = 1 << PAGE_BITS

    L0_SIZE = 4 # TTBR count
    L0_OFF = 30
    L1_OFF = 21
    L2_OFF = 12

    IDX_BITS = 9
    Lx_SIZE = (1 << IDX_BITS)
    IDX_MASK = Lx_SIZE - 1

    def __init__(self, iface, regs, util=None, compat="dart,s5l8960x"):
        self.iface = iface
        self.regs = regs
        self.u = util
        self.pt_cache = {}
        #self.enabled_streams = regs.ENABLED_STREAMS.val
        self.ptecls = PTE_TYPES[compat]

    @classmethod
    def from_adt(cls, u, path, instance=0, **kwargs):
        dart_addr = u.adt[path].get_reg(instance)[0]
        dart = cls(u.iface, dart_addr, u)
        dart.ptecls = PTE_TYPES[u.adt[path].compatible[0]]
        return dart

    def iomap_at(self, stream, iova, addr, size):
        if size == 0:
            return

        if not (self.enabled_streams & (1 << stream)):
            self.enabled_streams |= (1 << stream)

        if addr & (self.PAGE_SIZE - 1):
            raise Exception(f"Unaligned PA {addr:#x}")

        if iova & (self.PAGE_SIZE - 1):
            raise Exception(f"Unaligned IOVA {iova:#x}")

        start_page = align_down(iova, self.PAGE_SIZE)
        end = iova + size
        end_page = align_up(end, self.PAGE_SIZE)

        dirty = set()

        for page in range(start_page, end_page, self.PAGE_SIZE):
            paddr = addr + page - start_page

            l0 = page >> self.L0_OFF
            assert l0 < self.L0_SIZE
            ttbr = self.regs.TTBR[stream, l0].reg
            if not ttbr.VALID:
                l1addr = self.u.memalign(self.PAGE_SIZE, self.PAGE_SIZE)
                self.pt_cache[l1addr] = [0] * self.Lx_SIZE
                ttbr.VALID = 1
                ttbr.ADDR = l1addr >> 12
                self.regs.TTBR[stream, l0].reg = ttbr

            cached, l1 = self.get_pt(ttbr.ADDR << 12)
            l1idx = (page >> self.L1_OFF) & self.IDX_MASK
            l1pte = self.ptecls(l1[l1idx])
            if not l1pte.VALID:
                l2addr = self.u.memalign(self.PAGE_SIZE, self.PAGE_SIZE)
                self.pt_cache[l2addr] = [0] * self.Lx_SIZE
                l1pte = self.ptecls(
                    OFFSET=l2addr >> self.PAGE_BITS, VALID=3, WPROTECT=0)
                l1[l1idx] = l1pte.value
                dirty.add(ttbr.ADDR << 12)
            else:
                l2addr = l1pte.OFFSET << self.PAGE_BITS

            dirty.add(l1pte.OFFSET << self.PAGE_BITS)
            cached, l2 = self.get_pt(l2addr)
            l2idx = (page >> self.L2_OFF) & self.IDX_MASK
            self.pt_cache[l2addr][l2idx] = self.ptecls(
                OFFSET=paddr >> self.PAGE_BITS, VALID=3, WPROTECT=0).value

        for page in dirty:
            self.flush_pt(page)

    def iotranslate(self, stream, start, size):
        if size == 0:
            return []

        tcr_stream = R_TCR_STREAM((self.regs.TCR.val >> (idx * 8)) & 0xff)

        if not tcr_stream.TRANSLATE_ENABLE:
            return [(start, size)]

        #if tcr.BYPASS_DART or not tcr.TRANSLATE_ENABLE:
        #    raise Exception(f"Unknown DART mode {tcr}")

        start = start & 0xffffffff

        start_page = align_down(start, self.PAGE_SIZE)
        start_off = start - start_page
        end = start + size
        end_page = align_up(end, self.PAGE_SIZE)
        end_size = end - (end_page - self.PAGE_SIZE)

        pages = []

        for page in range(start_page, end_page, self.PAGE_SIZE):
            l0 = page >> self.L0_OFF
            assert l0 < self.L0_SIZE
            ttbr = self.regs.TTBR[stream, l0].reg
            if not ttbr.VALID:
                pages.append(None)
                continue

            cached, l1 = self.get_pt(ttbr.ADDR << 12)
            l1pte = self.ptecls(l1[(page >> self.L1_OFF) & self.IDX_MASK])
            if not l1pte.VALID and cached:
                cached, l1 = self.get_pt(ttbr.ADDR << 12, uncached=True)
                l1pte = self.ptecls(l1[(page >> self.L1_OFF) & self.IDX_MASK])
            if not l1pte.VALID:
                pages.append(None)
                continue

            cached, l2 = self.get_pt(l1pte.OFFSET << self.PAGE_BITS)
            l2pte = self.ptecls(l2[(page >> self.L2_OFF) & self.IDX_MASK])
            if not l2pte.VALID and cached:
                cached, l2 = self.get_pt(l1pte.OFFSET << self.PAGE_BITS, uncached=True)
                l2pte = self.ptecls(l2[(page >> self.L2_OFF) & self.IDX_MASK])
            if not l2pte.VALID:
                pages.append(None)
                continue

            pages.append(l2pte.OFFSET << self.PAGE_BITS)

        ranges = []

        for page in pages:
            if not ranges:
                ranges.append((page, self.PAGE_SIZE))
                continue
            laddr, lsize = ranges[-1]
            if ((page is None and laddr is None) or
                (page is not None and laddr == (page - lsize))):
                ranges[-1] = laddr, lsize + self.PAGE_SIZE
            else:
                ranges.append((page, self.PAGE_SIZE))

        ranges[-1] = (ranges[-1][0], ranges[-1][1] - self.PAGE_SIZE + end_size)

        if start_off:
            ranges[0] = (ranges[0][0] + start_off if ranges[0][0] else None,
                         ranges[0][1] - start_off)

        return ranges

    def get_pt(self, addr, uncached=False):
        cached = True
        if addr not in self.pt_cache or uncached:
            cached = False
            self.pt_cache[addr] = list(
                struct.unpack(f"<{self.Lx_SIZE}Q", self.iface.readmem(addr, self.PAGE_SIZE)))

        return cached, self.pt_cache[addr]

    def flush_pt(self, addr):
        assert addr in self.pt_cache
        self.iface.writemem(addr, struct.pack(f"<{self.Lx_SIZE}Q", *self.pt_cache[addr]))

    def initialize(self):
        self.regs.TCR.reg = R_TCR(TCR_0=R_TCR_STREAM(TRANSLATE_ENABLE=1).value, TCR_1=R_TCR_STREAM(TRANSLATE_ENABLE=1).value, TCR_2=R_TCR_STREAM(TRANSLATE_ENABLE=1).value, TCR_3=R_TCR_STREAM(TRANSLATE_ENABLE=1).value)

        for i in range(4):
            for j in range(4):
                self.regs.TTBR[i, j].reg = R_TTBR(VALID = 0)

        self.regs.ERROR.val = 0xffffffff
        self.enabled_streams = 0

        self.invalidate_streams()

    def show_error(self):
        if self.regs.ERROR.reg.FLAG:
            print(f"ERROR: {self.regs.ERROR.reg!s}")
            print(f"AXI_REQ0: {self.regs.ERROR_AXI_REQ0.reg!s}")
            print(f"AXI_REQ1: {self.regs.ERROR_AXI_REQ1.reg!s}")
            print(f"ADDR_LO: {self.regs.ERROR_ADDR_LO.val:#x}")
            self.regs.ERROR.val = 0xffffffff

    def invalidate_streams(self, streams=0x10):
        for sid in range(0,4):
            if streams & (1 << sid):
                self.regs.STREAM_COMMAND.val = R_STREAM_COMMAND(INVALIDATE=1).value | (1 << (sid + 8))
                while self.regs.STREAM_COMMAND.reg.BUSY:
                    pass

    def invalidate_cache(self):
        self.pt_cache = {}

    def dump_table2(self, base, l1_addr):

        def print_block(base, pte, start, last):
            pgcount = last - start
            pte.OFFSET -= pgcount
            print("    page (%4d): %08x ... %08x -> %016x [%d%d]" % (
                    start, base + start*0x1000, base + (start+1)*0x1000,
                    pte.OFFSET << self.PAGE_BITS, pte.WPROTECT, pte.VALID))
            if start < last:
                print("     ==> (%4d):          ... %08x -> %016x size: %08x" % (
                    last, base + (last+1)*0x1000,
                    (pte.OFFSET + pgcount - 1) << self.PAGE_BITS, pgcount << self.PAGE_BITS))

        cached, tbl = self.get_pt(l1_addr)

        unmapped = False
        start = 0
        next_pte = self.ptecls(VALID=0)

        for i, pte in enumerate(tbl):
            pte = self.ptecls(pte)
            if not pte.VALID:
                if not unmapped:
                    if next_pte.VALID:
                        print_block(base, next_pte, start, i)
                    print("  ...")
                    unmapped = True
                    next_pte = pte
                continue

            unmapped = False

            if int(pte) != int(next_pte):
                if next_pte.VALID:
                    print_block(base, next_pte, start, i)
                start = i

            next_pte = pte
            next_pte.OFFSET += 1

        if next_pte.VALID:
            print_block(base, next_pte, start, 2048)

    def dump_table(self, base, l1_addr):
        cached, tbl = self.get_pt(l1_addr)

        unmapped = False
        for i, pte in enumerate(tbl):
            pte = self.ptecls(pte)
            if not pte.VALID:
                if not unmapped:
                    print("  ...")
                    unmapped = True
                continue

            unmapped = False

            print("  table (%d): %08x ... %08x -> %016x [%d%d]" % (
                i, base + i*0x2000000, base + (i+1)*0x2000000,
                pte.OFFSET << self.PAGE_BITS, pte.WPROTECT, pte.VALID))
            self.dump_table2(base + i*0x2000000, pte.OFFSET << self.PAGE_BITS)

    def dump_ttbr(self, idx, ttbr):
        if not ttbr.VALID:
            return

        l1_addr = (ttbr.ADDR) << 12
        print("  TTBR%d: %09x" % (idx, l1_addr))

        self.dump_table(0, l1_addr)

    def dump_device(self, idx):
        assert(idx < 4)
        tcr_stream = R_TCR_STREAM((self.regs.TCR.val >> (idx * 8)) & 0xff)
        ttbrs = self.regs.TTBR[idx, :]
        print(f"dev {idx:02x}: TCR_{idx}={tcr_stream!s} TTBRs = [{', '.join(map(str, ttbrs))}]")

        if self.regs.TCR.val & (1 << (7 + 8 * idx)):
            print("  mode: TRANSLATE")

            for idx, ttbr in enumerate(ttbrs):
                self.dump_ttbr(idx, ttbr.reg)
        else:
            print("  mode: BYPASS")

    def dump_params(self):
        pass
