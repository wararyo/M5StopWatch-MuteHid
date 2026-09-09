"""Read/backup the StopWatch, or install only ota_1 after a full backup.

Normal writes preserve UserDemo, NVS, storage and partition table. --boot changes
only otadata as well. No full erase/initial-layout installation is implemented.
"""
import argparse
import binascii
import datetime
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SIZE = 0x1000000
SLOT = 0x510000
SLOT_SIZE = 0x4F0000
EXPECTED = {
    "nvs": (1, 2, 0x9000, 0x4000), "otadata": (1, 0, 0xD000, 0x2000),
    "phy_init": (1, 1, 0xF000, 0x1000), "ota_0": (0, 16, 0x20000, SLOT_SIZE),
    "ota_1": (0, 17, SLOT, SLOT_SIZE), "storage": (1, 129, 0xA00000, 0x400000),
    "coredump": (1, 3, 0xE00000, 0x10000),
}


def check_table(data):
    entries = {}
    for offset in range(0, len(data), 32):
        block = data[offset:offset + 32]
        if block[:2] == b"\xeb\xeb":
            if block[16:] != hashlib.md5(data[:offset]).digest():
                raise ValueError("Partition MD5 mismatch")
            if entries != EXPECTED:
                raise ValueError(f"Not the coexistence layout: {entries}")
            return
        if len(block) != 32 or block[:2] == b"\xff\xff":
            break
        magic, kind, subtype, addr, size, label, flags = struct.unpack("<HBBII16sI", block)
        name = label.rstrip(b"\0").decode()
        if magic != 0x50AA or flags or name in entries:
            raise ValueError("Invalid partition entry")
        entries[name] = kind, subtype, addr, size
    raise ValueError("Partition table has no verified MD5")


def app_name(data):
    if len(data) < 112 or data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        return "<invalid>"
    return data[80:112].split(b"\0", 1)[0].decode(errors="replace")


def ota_selection(slot):
    # Same CRC convention as ESP-IDF components/app_update/otatool.py.
    seq = struct.pack("<I", slot + 1)
    entry = seq + b"\xff" * 24 + struct.pack("<I", binascii.crc32(seq, 0xFFFFFFFF) & 0xFFFFFFFF)
    return entry + b"\xff" * (0x2000 - len(entry))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("action", choices=["backup", "install", "update"])
    p.add_argument("--port", required=True)
    p.add_argument("--baud", type=int, default=1500000)
    p.add_argument("--firmware", type=Path, default=ROOT / ".pio/build/m5stopwatch-coexist/firmware.bin")
    p.add_argument("--esptool", type=Path, default=Path.home() / ".platformio/packages/tool-esptoolpy/esptool.py")
    p.add_argument("--boot", action="store_true", help="Also select ota_1 after successful verification")
    p.add_argument("--backup", type=Path, help="Reuse a full backup only after SHA256 and live Flash verification")
    p.add_argument("--execute", action="store_true")
    args = p.parse_args()
    if args.action in ("install", "update"):
        fw = args.firmware.read_bytes()
        if not (0 < len(fw) <= SLOT_SIZE) or app_name(fw) != "M5StopWatch-MuteHid":
            raise ValueError("Not a fitting MuteHid firmware")
        check_table(args.firmware.with_name("partitions.bin").read_bytes())
        print(f"PLAN: verify original backup; write {len(fw)} bytes to ota_1 at 0x510000; boot={args.boot}")
        if not args.execute:
            print("DRY RUN. Add --execute to back up and write the device.")
            return
    base = [sys.executable, str(args.esptool), "--chip", "esp32s3", "--port", args.port, "--baud", str(args.baud)]
    def run(*tail):
        subprocess.run(base + list(map(str, tail)), check=True)
    directory = ROOT / ".pio/phase0/backups"
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    backup = args.backup or directory / f"before-phase0-{stamp}.bin"
    if args.action == "update" and not args.backup:
        raise ValueError("update requires the original full backup via --backup")
    if not args.backup:
        run("--after", "no_reset", "read_flash", "0", hex(SIZE), backup)
    data = backup.read_bytes()
    if len(data) != SIZE:
        raise ValueError("Incomplete backup; no writes performed")
    sha = hashlib.sha256(data).hexdigest()
    if args.backup:
        if backup.with_suffix(".sha256").read_text().strip() != sha:
            raise ValueError("Backup SHA256 mismatch")
        # Device-side MD5 avoids a second 16 MB serial transfer and ensures
        # this backup still exactly describes the connected device.
        if args.action == "update":
            # The original pre-PoC image remains the recovery point. Verify
            # its bootloader/table/UserDemo still match; the current PoC may
            # differ, and NVS must retain newly created BLE bonds.
            protected = directory / f"protected-{stamp}.bin"
            protected.write_bytes(data[:0x9000])
            userdemo = directory / f"userdemo-{stamp}.bin"
            userdemo.write_bytes(data[0x20000:0x510000])
            run("--after", "no_reset", "verify_flash", "0", protected, "0x20000", userdemo)
            current = directory / f"current-app-{stamp}.bin"
            run("--after", "no_reset", "read_flash", hex(SLOT), "0x1000", current)
            if app_name(current.read_bytes()) != "M5StopWatch-MuteHid":
                raise ValueError("update only replaces an existing MuteHid image")
        else:
            run("--after", "no_reset", "verify_flash", "0", backup)
    else:
        backup.with_suffix(".sha256").write_text(sha + "\n")
    check_table(data[0x8000:0x9000])
    info = {"backup": str(backup), "sha256": sha, "bytes": len(data),
            "ota_0": app_name(data[0x20000:]), "ota_1": app_name(data[SLOT:]), "port": args.port}
    backup.with_suffix(".json").write_text(json.dumps(info, indent=2) + "\n")
    print(json.dumps(info, indent=2))
    if args.action == "backup":
        run("run")
        return
    if info["ota_0"] != "StopWatch-UserDemo":
        raise ValueError("UserDemo not found; refusing to overwrite ota_1")
    run("--after", "no_reset", "write_flash", "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB", hex(SLOT), args.firmware)
    run("--after", "no_reset", "verify_flash", hex(SLOT), args.firmware)
    if args.boot:
        selection = directory / f"boot-mutehid-{stamp}.bin"
        selection.write_bytes(ota_selection(1))
        run("write_flash", "0xd000", selection)
    else:
        run("run")
    print("Installed and verified. Original firmware and otadata are in the full backup.")


if __name__ == "__main__":
    main()
