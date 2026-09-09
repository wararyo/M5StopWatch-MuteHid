"""Discover MuteHid advertisements without connecting to other devices."""
import asyncio
import json
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / ".pio/host-tools"))
from bleak import BleakScanner

async def main():
    devices = await BleakScanner.discover(timeout=10, return_adv=True)
    matches = [{"address": d.address, "name": a.local_name or d.name,
                "rssi": a.rssi, "services": a.service_uuids}
               for d, a in devices.values() if "MuteHid" in (a.local_name or d.name or "")]
    print(json.dumps(matches, indent=2))

asyncio.run(main())

