#!/usr/bin/env python3
"""Modbus RTU slave simulator for the end-to-end test (pymodbus >= 3.12).

Unit 1 exposes the registers used by e2e_config.json:

  input   0-1  Voltage  float32 ABCD, starts at 230.0
  holding 10   Setpoint int16 (raw, scale 0.1 in the gateway)
  holding 20-21 Energy  uint32 CDAB = 0x12345678
  coil    0    Run
  discrete 2   Alarm (true)

Every write from the gateway is printed as
"WRITE fc=<n> address=<a> values=[...]" so the harness can assert on it.
With --wobble the Voltage changes every second, alternately by +0.05
(inside the 0.1 deadband) and by +0.5 (outside it), printing
"VOLTAGE <value>" each time.
"""

import argparse
import asyncio
import struct
import sys

from pymodbus.server import StartAsyncSerialServer
from pymodbus.simulator import DataType, SimData, SimDevice

state = {"voltage": 230.0}


def float_regs(value):
    return list(struct.unpack(">HH", struct.pack(">f", value)))


async def action(function_code, start_address, address, count, current_registers,
                 set_values):
    """Log writes; serve the current Voltage on FC04."""
    if set_values is not None:
        print(f"WRITE fc={function_code} address={address} values={list(set_values)}",
              flush=True)
    elif function_code == 4 and start_address == 0:
        current_registers[0:2] = float_regs(state["voltage"])
    return None


async def wobble():
    step = 0
    while True:
        await asyncio.sleep(1.0)
        state["voltage"] += 0.05 if step % 2 == 0 else 0.5
        step += 1
        print(f"VOLTAGE {state['voltage']:.2f}", flush=True)


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=9600)
    parser.add_argument("--wobble", action="store_true")
    args = parser.parse_args()

    device = SimDevice(
        id=1,
        simdata=(
            [SimData(0, count=16, values=False, datatype=DataType.BITS)],  # coils
            [SimData(0, values=[False, False, True] + [False] * 13,       # discrete,
                     datatype=DataType.BITS)],                            # Alarm = 2
            [SimData(0, count=20, values=0, datatype=DataType.REGISTERS),  # holding
             SimData(20, values=[0x5678, 0x1234], datatype=DataType.REGISTERS),
             SimData(22, count=18, values=0, datatype=DataType.REGISTERS)],
            [SimData(0, count=40, values=0, datatype=DataType.REGISTERS)],  # input
        ),
        action=action,
    )
    if args.wobble:
        asyncio.get_running_loop().create_task(wobble())
    print("SLAVE READY", flush=True)
    await StartAsyncSerialServer(context=device, port=args.port, baudrate=args.baud,
                                 bytesize=8, parity="N", stopbits=1)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
