"""Block the stock flashing targets on a coexistence device.

`upload` writes the bootloader, the partition table and the application at the
image offset (0x10000 here), which runs straight through ota_0 and destroys
UserDemo; `erase` also takes NVS with the BLE bond and the shared settings.
The safe path is `pio run -t update`, which only replaces ota_1.
"""
Import("env")

BLOCKED = ("upload", "uploadfs", "uploadfsota", "erase")

if any(target in COMMAND_LINE_TARGETS for target in BLOCKED):
    raise RuntimeError(
        "This target overwrites UserDemo or NVS. Use `pio run -t update` "
        "(tools/device.py) to write only ota_1 after verifying the backup.")
