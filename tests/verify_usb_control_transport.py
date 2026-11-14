"""Verify that USB output reports use EP0 instead of an OUT endpoint."""

from pathlib import Path
import re


root = Path(__file__).resolve().parents[1]
transport = (root / "src" / "ControlOnlyUsbHid.cpp").read_text(encoding="utf-8")
codex = (root / "src" / "CodexMicroBle.cpp").read_text(encoding="utf-8")

assert "TUD_HID_DESCRIPTOR" in transport
assert "TUD_HID_INOUT_DESCRIPTOR" not in transport
assert "tinyusb_get_free_in_endpoint" in transport
assert "tinyusb_get_free_out_endpoint" not in transport
assert re.search(
    r"tinyusb_enable_interface\(USB_INTERFACE_HID,\s*TUD_HID_DESC_LEN",
    transport,
)
assert "tud_hid_set_report_cb" in transport
assert "outputCallback(outputContext, reportId, buffer, bufferSize)" in transport

assert 'kFirmwareVersion[] = "0.1.12-direct5v"' in codex
assert 'kUsbSerial[] = "2884851ECB78"' in codex
assert "USB.usbAttributes(0);" in codex
assert "USB.usbPower(500);" in codex
assert "USB resume recovery: forcing HID re-enumeration" in codex
assert "tud_disconnect()" in codex
assert "tud_connect();" in codex
assert codex.index("USB.usbAttributes(0);") < codex.index("USB.begin()")
assert "USBHID" not in codex
assert "serviceUsbRecovery" not in codex
assert "usbSerialVariant" not in codex

report_map = re.search(
    r"const uint8_t kReportMap\[\] = \{(.*?)\n\};", codex, re.S
).group(1)
assert "0x85, 0x06" in report_map
assert "0x95, 0x3F" in report_map
assert "0x81, 0x02" in report_map
assert "0x91, 0x02" in report_map

firmware_map = root / ".pio" / "build" / "esp32-s3-n16r8" / "firmware.map"
if firmware_map.exists():
    linked = firmware_map.read_text(encoding="utf-8", errors="replace")
    assert "src\\ControlOnlyUsbHid.cpp.o" in linked
    assert "libraries\\USB\\src\\USBHID.cpp.o" not in linked
    assert "tud_hid_set_report_cb" in linked

print("PASS: HID keeps IN 0x81 and routes host output reports over EP0 SET_REPORT")
