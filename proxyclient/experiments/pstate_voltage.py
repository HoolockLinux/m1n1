#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib, time
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1 import asm

CREG = 0x202f00000
PSINFO1 = 0x80000
PSINFO1_IS_PCORE = 1 << 23

PSINFO2 = 0x80008
OSC_FREQ = 24000000
MAX_PSTATE = 11

# PSINFO1[23], Core type indicator
# PSINFO1[14:16], CPU Ratio 1
# PSINFO1[4:11], CPU Ratio 2
# PSINFO1[0:3], CPU Ratio 3
#


for N in range(0, MAX_PSTATE + 1):
	psinfo1 = p.read64(0x202f80000 + N * 0x20)
	unk2_volt = (((psinfo1 >> 0x38) & 0xff) * 0xc35 - 500) / 1000 + 500
	#cpu_ratio1 = ((psinfo1 & 0xffffffff) >> 0xe) & 7)
	#cpu_ratio2 = 
	#cpu_ratio3
	ratio = (((psinfo1 & 0xffffffff) >> 0xe) & 7) / 5 + ((psinfo1 >> 4) & 0xff) / ((psinfo1 & 0xf) * 2 + 2)
	print("Cpu Ratio:", ratio)
	freq = ratio * OSC_FREQ
	#freq = ((((psinfo1 & 0xffffffff) >> 0xe) & 7) / 5 + ((psinfo1 >> 4) & 0xff) * OSC_FREQ) / ((psinfo1 & 0xf) * 2 + 2)
	psinfo2 = p.read64(CREG + PSINFO2 + N * 0x20)
	unk1_volt = (((psinfo2 >> 0x20) & 0xff) * 0xc35 - 600) / 1000 + 600
	print("Cpu State:", N)
	print("Cpu Core:", "pCore" if psinfo1 & PSINFO1_IS_PCORE else "eCore")
	print("Cpu Frequency:", int(freq / 1000000) , "MHz")
	print("Unknown Voltage 1:", unk1_volt, "mV")
	print("Unknown Voltage 2:", unk2_volt, "mV")
	print("psinfo1:", hex(psinfo1))
	print("psinfo2:", hex(psinfo2))
	print()
	print()
