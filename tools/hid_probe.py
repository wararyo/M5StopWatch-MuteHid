"""Windows HID diagnostic (distinct from Google Meet compatibility).

Install hidapi in .pio/host-tools. Enumeration is read-only; --output writes
the Mute LED report to this device, not to the OS microphone endpoint.
"""
import argparse
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".pio/host-tools"))
import hid

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--output", type=int, choices=[0, 1])
p.add_argument("--listen", type=float, default=0)
p.add_argument("--serial", help="Required when more than one MuteHid device is found")
a = p.parse_args()
devices = [d for d in hid.enumerate() if d.get("usage_page") == 0x0B and d.get("usage") == 1
           and "MuteHid" in (d.get("product_string") or "")]
if a.serial:
    devices = [d for d in devices if d.get("serial_number") == a.serial]
print(json.dumps(devices, indent=2, default=lambda v: v.decode(errors="replace") if isinstance(v, bytes) else str(v)), flush=True)
if a.output is not None or a.listen:
    if len(devices) != 1:
        raise SystemExit(f"Expected exactly one MuteHid Telephony collection, found {len(devices)}")
    dev = hid.device()
    dev.open_path(devices[0]["path"])
    try:
        if a.output is not None:
            report = bytes([1, a.output])  # Windows report ID + one-byte payload.
            count = dev.write(report)
            print(f"OUTPUT {report.hex()} accepted_bytes={count}", flush=True)
            if count != len(report):
                raise RuntimeError("Incomplete HID output write")
        end = time.monotonic() + a.listen
        while time.monotonic() < end:
            data = dev.read(64, 200)
            if data:
                print(f"INPUT {bytes(data).hex()} time={time.time():.3f}", flush=True)
    finally:
        dev.close()

