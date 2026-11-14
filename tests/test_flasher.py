import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("flasher", ROOT / "flasher" / "flasher.py")
flasher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flasher)


class FirmwareTests(unittest.TestCase):
    def test_actual_build_images(self):
        self.assertEqual(flasher.firmware_address(ROOT / "dist/reCodex-no-joystick-app.bin"), "0x10000")
        self.assertEqual(flasher.firmware_address(ROOT / "dist/reCodex-no-joystick-merged.bin"), "0x0")

    def test_merged_contains_exact_application(self):
        app = (ROOT / "dist/reCodex-no-joystick-app.bin").read_bytes()
        merged = (ROOT / "dist/reCodex-no-joystick-merged.bin").read_bytes()
        self.assertEqual(merged[0x10000:0x10000 + len(app)], app)

    def test_invalid_images(self):
        app = bytearray((ROOT / "dist/reCodex-no-joystick-app.bin").read_bytes())
        app[12:14] = b"\x00\x00"
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "bad.bin"
            for data in [b"", b"not firmware" * 100, app]:
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    flasher.firmware_address(path)

    def test_bootloader_not_misidentified_as_merged(self):
        with self.assertRaises(ValueError):
            flasher.firmware_address(ROOT / ".pio/build/esp32-s3-n16r8/bootloader.bin")

    def test_filename_does_not_override_address(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "misleading-merged.bin"
            path.write_bytes((ROOT / "dist/reCodex-no-joystick-app.bin").read_bytes())
            self.assertEqual(flasher.firmware_address(path), "0x10000")


if __name__ == "__main__":
    unittest.main()
