# SPDX-License-Identifier: MIT
from .base import *
from ...utils import *
from construct import *
from ...sysreg import *
from enum import IntEnum

class SPSR_M_V7(IntEnum):
    Unknown = 0x0 # some versions don't have this value
    User = 0x10
    FIQ = 0x11
    IRQ = 0x12
    Supervisor = 0x13
    Monitor = 0x16
    Abort = 0x17
    Hyp = 0x1a
    Undefined = 0x1b
    System = 0x1f

class SPSR_V7(Register32):
    N = 31
    Z = 30
    C = 29
    V = 28
    Q = 27
    IT_HI = 26, 25
    J = 24
    GE = 19, 16
    IT_LO = 15, 10
    E = 9
    A = 8
    I = 7
    F = 6
    T = 5
    M = 4, 0, SPSR_M_V7

class CrashLogMessage(Register64):
    EP = 63, 56, Constant(1)
    TYPE = 55, 52
    SIZE = 51, 44
    DVA = 43, 0

CrashHeader = Struct(
    "type" / Const("CLHE", FourCC),
    "ver" / Int32ul,
    "total_size" / Int32ul,
    "flags" / Int32ul,
    Padding(16)
)

CrashCver = Struct(
    "uuid" / Bytes(16),
    "version" / CString("utf8"),
)

CrashCstr = Struct(
    "id" / Int32ul,
    "string" / CString("utf8"),
)

CrashCtim = Struct(
    "time" / Int64ul,
)

CrashCrgA = Struct(
    "unk_0" / Int32ul,
    "unk_4" / Int32ul,
    "regs" / Array(16, Hex(Int32ul)),
    "spsr" / Int32ul,
    "unk_x1" / Int32ul,
    "stack" / Bytes(0x100),
    "unk_x2" / Int32ul,
    "unk_x3" / Int32ul,
    "unk_x4" / Int32ul,
    "unk_x5" / Int32ul,
)

CrashCmbx = Struct(
    "hdr" / Array(4, Hex(Int32ul)),
    "type" / Int32ul,
    "unk" / Int32ul,
    "index" / Int32ul,
    "messages" / GreedyRange(Struct(
        "endpoint" / Hex(Int64ul),
        "message" / Hex(Int64ul),
        "timestamp" / Hex(Int32ul),
        Padding(4),
    )),
)

CrashCcst = Struct(
    "task" / Int32ul,
    "unk" / Int32ul,
    "stack" / GreedyRange(Int64ul)
)

CrashCasC = Struct(
    "l2c_err_sts" / Hex(Int64ul),
    "l2c_err_adr" / Hex(Int64ul),
    "l2c_err_inf" / Hex(Int64ul),
    "lsu_err_sts" / Hex(Int64ul),
    "fed_err_sts" / Hex(Int64ul),
    "mmu_err_sts" / Hex(Int64ul)
)

CrtkEntry = Struct(
    "id" / Int16ul,
    "flags" / Int16ul,
    "unk_4" / Int32ul,
    "unk_8" / Int32ul,
    "unk_c" / Int32ul,
    "ptr" / Int64ul,
    "unk_14" / Int32ul,
    "unk_18" / Int32ul,
    "unk_1c" / Int32ul,
    "unk_20" / Int32ul,
    "ptr_count" / Int32ul,
    "name" / Bytes(0x1c),
    "ptrs" / Array(this.ptr_count, Int64ul)
)

CrashCrtk = Struct(
    "tasks" / GreedyRange(CrtkEntry),
    "leftovers" / GreedyBytes,
)

CcdpEntry = Struct(
    "va" / Int64ul,
    "pa" / Int64ul,
    "unk_10" / Int32ul,
)

CrashCcdp = Struct(
    "entries" / GreedyRange(CcdpEntry),
    "leftovers" / GreedyBytes,
)

CrashEntry = Struct(
    "type" / FourCC,
    Padding(4),
    "flags" / Hex(Int32ul),
    "len" / Int32ul,
    "payload" / FixedSized(lambda ctx: ctx.len - 16 if ctx.type != "CLHE" else 16,
                           Switch(this.type, {
        "Cver": CrashCver,
        "Ctim": CrashCtim,
        "Cmbx": CrashCmbx,
        "CrgA": CrashCrgA,
        "Cstr": CrashCstr,
        "Ccst": CrashCcst,
        "CasC": CrashCasC,
        "Crtk": CrashCrtk,
        "Ccdp": CrashCcdp,
    }, default=GreedyBytes)),
)

CrashLog = Struct(
    "header" / CrashHeader,
    "entries" / RepeatUntil(this.type == "CLHE", CrashEntry),
)

class CrashLogParser:
    def __init__(self, data=None, akf=None):
        self.akf = akf
        self.task_id = None
        if data is not None:
            self.parse(data)

    def parse(self, data):
        self.data = CrashLog.parse(data)
        pass

    def default(self, entry):
        print(f"# {entry.type} flags={entry.flags:#x}")
        chexdump(entry.payload)
        print()

    def Ccst(self, entry):
        self.task_id = entry.payload.task
        print(f"Call stack (task {entry.payload.task}):")
        for i in entry.payload.stack:
            if not i:
                break
            print(f"  - {i:#x}")
        print()

    def CasC(self, entry):
        print(f"Async error info:")
        print(entry.payload)
        print()

    def Cver(self, entry):
        print(f"RTKit Version: {entry.payload.version}")
        print()

    def Cstr(self, entry):
        print(f"Message {entry.payload.id}: {entry.payload.string}")
        print()

    def Ctim(self, entry):
        print(f"Crash time: {entry.payload.time:#x}")
        print()

    def Cmbx(self, entry):
        print(f"Mailbox log (type {entry.payload.type}, index {entry.payload.index}):")
        for i, msg in enumerate(entry.payload.messages):
            print(f" #{i:3d} @{msg.timestamp:#10x} ep={msg.endpoint:#4x} {msg.message:#18x}")
        print()

    def CrgA(self, entry):
        print(f"Exception info:")

        ctx = entry.payload

        spsr = SPSR_V7(ctx.spsr)

        print(f"  == Exception taken from {spsr.M.name} Mode ==")
        print(f"  SPSR   = {spsr}")
        print(f"  SP     = {ctx.regs[13]:#x}")

        for i in range(0, 16, 4):
            j = min(15, i + 3)
            print(f"  {f'r{i}-r{j}':>7} = {' '.join(f'{r:08x}' for r in ctx.regs[i:j + 1])}")

        print()

    def CLHE(self, entry):
        # terminator?
        pass

    def Crtk(self, entry):
        print("Tasks:")
        for task in entry.payload.tasks:
            name = task.name.rstrip(b'\x00').decode()
            if self.task_id == task.id:
                print(f"* {task.id}: {name} (flags 0x{task.flags:x})")
            else:
                print(f"  {task.id}: {name} @ {task.ptr:08x} (flags 0x{task.flags:x})")
                print(f"   Stack: ", end="")
                for p in task.ptrs:
                    print(f"0x{p:08x}", end=" ")
                print("")
            print(f"   Unknowns: {task.unk_4:08x} {task.unk_8:08x} {task.unk_c:08x} {task.unk_14:08x} {task.unk_18:08x} {task.unk_1c:08x} {task.unk_20:08x}")
        if len(entry.payload.leftovers):
            chexdump(entry.payload.leftovers)
        print()

    def Ccdp(self, entry):
        print("Ccdp:")
        for e in entry.payload.entries:
            real_pa = self.akf.iotranslate(e.va, 1)[0][0]
            name = self.akf.addr(e.va) if e.va else ""
            name = "("+name+")" if ('@' in name) else ""
            print(f" 0x{e.va:016x} -> 0x{e.pa:016x} {name} : {e.unk_10:08x}")
            if real_pa != e.pa:
                if real_pa != None:
                    print(f"  (wrong PA; should be 0x{real_pa:16x})")
                else:
                    print(f"  (wrong PA; not mapped)")
        print()

    def dump(self):
        print("### Crash dump:")
        print()
        for entry in self.data.entries:
            getattr(self, entry.type, self.default)(entry)

class AKFCrashLogEndpoint(AKFBaseEndpoint):
    SHORT = "crash"
    BASE_MESSAGE = CrashLogMessage

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.iobuffer = None
        self.iobuffer_dva = None
        self.started = False

    @msg_handler(0x1)
    def Handle(self, msg):
        if self.started:
            return self.handle_crashed(msg)
        else:
            return self.handle_getbuf(msg)

    def handle_getbuf(self, msg):
        if msg.DVA:
            size = 0x1000 * msg.SIZE
            self.iobuffer_dva = msg.DVA
            self.log(f"buf prealloc at dva {self.iobuffer_dva:#x}")
        else:
            size = align(0x1000 * msg.SIZE, 0x4000)
            self.iobuffer, self.iobuffer_dva = self.akf.ioalloc(size)
            self.log(f"buf {self.iobuffer:#x} / {self.iobuffer_dva:#x}")
            self.send(CrashLogMessage(TYPE=1, SIZE=size // 0x1000, DVA=self.iobuffer_dva))

        self.started = True
        return True

    def crash_soft(self):
        self.send(0x40)

    def crash_hard(self):
        self.send(0x22)

    def handle_crashed(self, msg):
        size = 0x1000 * msg.SIZE

        self.log(f"Crashed!")
        self.log(f" DVA @ 0x{msg.DVA:x}")
        crashdata = self.akf.ioread(msg.DVA, size)
        open("crash.bin", "wb").write(crashdata)
        clog = CrashLogParser(crashdata, self.akf)
        clog.dump()
        raise Exception("AKF crashed!")

        return True

if __name__ == "__main__":
    import sys
    crashdata = open(sys.argv[1], "rb").read()
    clog = CrashLogParser(crashdata)
    clog.dump()
