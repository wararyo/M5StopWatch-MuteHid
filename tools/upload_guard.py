"""Block the stock flashing targets on the coexistence build.

In that build `upload` writes the bootloader, the partition table and the
application at the image offset (0x10000), which runs straight through ota_0 and
destroys UserDemo; `erase` also takes NVS with the BLE bond and the shared
settings. The safe path is `pio run -t update`, which only replaces the selected app slot.

The standalone build owns the whole device, so its upload is left alone. It
still rewrites the partition table, which is worth saying out loud when the
device currently holds the coexistence layout.
"""
Import("env")

BLOCKED = ("upload", "uploadfs", "uploadfsota", "erase")
requested = [target for target in BLOCKED if target in COMMAND_LINE_TARGETS]

if requested and env["PIOENV"] == "m5stopwatch-coexist":
    raise RuntimeError(
        "This target overwrites UserDemo or NVS. Use `pio run -t update` "
        "(tools/device.py) to write only the selected app slot after verifying the backup.")

if requested:
    print("NOTE: the standalone build replaces the partition table; a device set "
          "up for coexistence loses access to UserDemo.")
