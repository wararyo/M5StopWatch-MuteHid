import hashlib
from pathlib import Path
import struct
import unittest
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


if __name__ == "__main__": unittest.main()
