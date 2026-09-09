"""Capture USB serial logs; optionally send a sequence of diagnostic characters."""
import argparse
from pathlib import Path
import time
import serial

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--port", required=True)
p.add_argument("--seconds", type=float, default=15)
p.add_argument("--commands", default="")
p.add_argument("--interval", type=float, default=2)
p.add_argument("--log", type=Path)
p.add_argument("--reset", action="store_true")
a = p.parse_args()
if a.log:
    a.log.parent.mkdir(parents=True, exist_ok=True)
log = a.log.open("ab") if a.log else None
try:
    with serial.Serial(port=None, baudrate=115200, timeout=0.1) as port:
        # USB Serial/JTAG interprets RTS/DTR as reset/download signals.
        port.rts = False
        port.dtr = False
        port.port = a.port
        port.open()
        if a.reset:
            port.rts = True
            port.dtr = False
            time.sleep(0.2)
            port.rts = False
            port.dtr = False
            time.sleep(0.2)
        port.dtr = True
        port.rts = False
        start = time.monotonic()
        next_send = start + 1
        commands = iter(a.commands)
        done = False
        while time.monotonic() - start < a.seconds:
            if not done and time.monotonic() >= next_send:
                c = next(commands, None)
                if c is None:
                    done = True
                else:
                    port.write(c.encode("ascii"))
                    print(f"[SERIAL COMMAND {c}]", flush=True)
                    next_send += a.interval
            chunk = port.read(max(1, port.in_waiting))
            if chunk:
                print(chunk.decode("utf-8", errors="replace"), end="", flush=True)
                if log:
                    log.write(chunk)
                    log.flush()
finally:
    if log:
        log.close()
