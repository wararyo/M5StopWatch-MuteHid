import hashlib
from pathlib import Path
import struct
import unittest
import tempfile
from unittest.mock import patch
import device


class FlashGuardTest(unittest.TestCase):
    def table(self):
        result = b"".join(struct.pack("<HBBII16sI", 0x50AA, kind, sub, addr, size, name.encode(), 0)
                          for name, (kind, sub, addr, size) in device.EXPECTED.items())
        return result + b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(result).digest()

    def test_valid(self):
        device.check_table(self.table())

    def test_corruption(self):
        data = bytearray(self.table()); data[9] ^= 1
        with self.assertRaises(ValueError): device.check_table(data)

    def test_no_checksum(self):
        with self.assertRaises(ValueError): device.check_table(self.table()[:-32])

    def test_wrong_layout_even_with_valid_checksum(self):
        data = bytearray(self.table()); data[9] ^= 1
        data[-16:] = hashlib.md5(data[:-32]).digest()
        with self.assertRaises(ValueError): device.check_table(data)

    def test_invalid_image(self):
        self.assertEqual(device.app_name(b"\xff" * 256), "<invalid>")

    def test_ota_selection(self):
        data = device.ota_selection(1)
        self.assertEqual(len(data), 8192)
        self.assertEqual(struct.unpack_from("<I", data)[0], 2)
        self.assertEqual(data[4096:], b"\xff" * 4096)
        self.assertEqual(struct.unpack_from("<I", data, 28)[0], 0x55F63774)

    def test_reject_legacy_two_app_layout(self):
        legacy = dict(device.EXPECTED)
        legacy.pop("ota_2")
        legacy.pop("ota_3")
        legacy["ota_1"] = (0, 17, 0x510000, 0x4F0000)
        with patch.object(device, "EXPECTED", legacy):
            table = self.table()
        with self.assertRaises(ValueError):
            device.check_table(table)

    def test_oversized_firmware_never_accesses_device(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            firmware = root / "firmware.bin"
            firmware.write_bytes(b"\xff" * (device.SLOT_SIZE + 1))
            argv = ["device.py", "install", "--slot", "2", "--port", "TEST",
                    "--esptool", str(firmware), "--firmware", str(firmware), "--execute"]
            with patch.object(device, "ROOT", root), patch("sys.argv", argv), \
                    patch.object(device.subprocess, "run") as run:
                with self.assertRaisesRegex(ValueError, "Not a fitting"):
                    device.main()
                run.assert_not_called()

    def test_install_and_update_each_slot(self):
        def app(name):
            data = bytearray(256)
            data[0] = 0xE9
            struct.pack_into("<I", data, 32, 0xABCD5432)
            data[80:80 + len(name)] = name.encode()
            return bytes(data)

        for action in ("install", "update"):
            for slot, address in device.SLOT_OFFSETS.items():
                with self.subTest(action=action, slot=slot), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    firmware = root / "firmware.bin"
                    firmware.write_bytes(app("M5StopWatch-MuteHid"))
                    (root / "partitions.bin").write_bytes(self.table())
                    backup = root / "before.bin"
                    data = bytearray(b"\xff" * device.SIZE)
                    table = self.table()
                    data[0x8000:0x8000 + len(table)] = table
                    data[0x20000:0x20100] = app("StopWatch-UserDemo")
                    backup.write_bytes(data)
                    backup.with_suffix(".sha256").write_text(hashlib.sha256(data).hexdigest())
                    calls = []

                    def run(command, check):
                        calls.append(command)
                        if "read_flash" in command:
                            index = command.index("read_flash")
                            self.assertEqual(command[index + 1], hex(address))
                            Path(command[-1]).write_bytes(app("M5StopWatch-MuteHid"))

                    argv = ["device.py", action, "--slot", str(slot), "--port", "TEST",
                            "--esptool", str(firmware), "--firmware", str(firmware),
                            "--backup", str(backup), "--boot", "--execute"]
                    with patch.object(device, "ROOT", root), patch("sys.argv", argv), \
                            patch.object(device.subprocess, "run", side_effect=run), patch("builtins.print"):
                        device.main()
                    writes = [command for command in calls if "write_flash" in command]
                    self.assertEqual(len(writes), 2)
                    self.assertEqual(writes[0][-2:], [hex(address), str(firmware)])
                    self.assertEqual(writes[1][-2], "0xd000")
                    selection = Path(writes[1][-1]).read_bytes()
                    self.assertEqual(selection, device.ota_selection(slot))


if __name__ == "__main__": unittest.main()
