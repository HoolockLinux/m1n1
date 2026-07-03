#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1.fw.ans1 import ANSClient
from m1n1.shell import run_shell

p.pmgr_adt_power_enable("/arm-io/ans")

ans = ANSClient(u, adt_path="/arm-io/ans")

if len(sys.argv) == 2 and sys.argv[1] == "stop":
    ans.stop(state=0x1)
    exit(0)

ans.start()
#ans.start_ep(0x20)
ansep = ans.epmap[0x20]

while ans.epmap[0].ap_power_state != 0x20:
    ans.work()

ansep.start_io()

run_shell(globals(), msg="Have fun!")

ans.stop(state=0x1)
