#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, pathlib, time
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from m1n1.setup import *
from m1n1 import asm

CREG = 0x202200000
PSINFO = 0x20068
# status [0:2] target
# status [5:3] current
STATUS = 0x20050
PLL_SCR1 = 0x200c8
PSINFO_PLL_CLOCK = 1 << 21 # Otherwise 24 MHz clock
PSINFO_DISABLE_VOLTAGE_CHANGE = 1 << 22
PSINFO_BYPASS = 1 << 23

MAX_PSTATE = 7

print(" == Current state ==")
pllctl = p.read32(CREG + PLL_SCR1)
state = (p.read32(CREG + STATUS) >> 3) & 7
p = (pllctl >> 9) & 0x1f
m = (pllctl >> 18) & 0x1ff
s = (pllctl >> 0) & 0xf
freq=((24000000*m)/p)/(s+1)

print("Cpu State:", state)
print("Cpu Frequency:", freq)
print()
print()


for N in [4]:
	psinfo = p.read64(CREG + PSINFO + N * 0x20)
	p = (psinfo >> 13) & 0x1f
	m = (psinfo >> 4) & 0x1ff
	s = (psinfo >> 0) & 0xf
	freq=((24000000*m)/p)/(s+1)

	volt = (psinfo >> 56) & 0xff
	volt1 = (psinfo >> 30) & 0x3f
	volt2 = (psinfo >> 36) & 0x3f
	volt3 = (psinfo >> 42) & 0x3f

	print("Cpu State:", N)
	print("Cpu Frequency:", freq)
	print("Cpu Base Voltage:", volt)
	print("Cpu Voltage Offset 1:", volt1)
	print("Cpu Voltage Offset 2:", volt2)
	print("Cpu Voltage Offset 3:", volt3)
	print()
	print()
