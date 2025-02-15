from m1n1.setup import *
from m1n1.hw.i2c import I2C
from datetime import datetime

compat = u.adt["/arm-io/i2c0/pmu"].compatible[0]
reg = u.adt["/arm-io/i2c0/pmu"].reg[0]

if compat in ["pmu,d2255", "pmu,d2257", "pmu,d2333", "pmu,d2365", "pmu,d2400"]:
    NVMEM = 0x5004
    RTC = 0x502
elif compat in ["pmu,d2045", "pmu,d2089", "pmu,d2186", "pmu,d2207"]:
    RTC = 0x5c6
    NVMEM = 0x4004
else:
    raise Exception("Unknown PMU compatible")

p.pmgr_adt_clocks_enable("/arm-io/i2c0")

i2c0 = I2C(u, "/arm-io/i2c0")

nvmem = i2c0.read_reg(reg, NVMEM, 0x4, regaddrlen=2)
time_base = (nvmem[0] | (nvmem[1] << 8) | (nvmem[2] << 16) | (nvmem[3] << 24))

print("time base", datetime.fromtimestamp(time_base))

def read_time():
    data = i2c0.read_reg(reg, RTC, 0x6, regaddrlen=2)
    time_off = (data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24) | (data[4] << 32) | (data[5] << 40))
    time_off_secs = time_off >> 16
    time_off_subsec = (time_off & 0xffff) / 0xffff
    return time_base + time_off_secs + time_off_subsec

for i in range(0,5*2):
    print("T+", i/2, datetime.fromtimestamp(read_time()))
    p.udelay(500000)
