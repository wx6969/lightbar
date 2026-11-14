"""Verify that the complete U7-U14 ambient chain is rendered and transmitted."""
from pathlib import Path
import re


source = (Path(__file__).resolve().parents[1] / "src" / "main.cpp").read_text(
    encoding="utf-8"
)

key_led_map = re.search(
    r"kPhysicalKeyToSerial\[kKeyLedCount\]\s*=\s*\{([^}]+)\}", source
).group(1)
assert [int(value) for value in re.findall(r"\d+", key_led_map)] == list(range(6))

assert re.search(r"kAmbientLedCount\s*=\s*8\s*;", source)
assert re.search(
    r"rmt_item32_t\s+ambientRmtItems\[kAmbientLedCount\s*\*\s*kBitsPerPixel\]",
    source,
)
assert re.search(
    r"sendFrame\(kAmbientLedChannel,\s*ambientFrame,\s*kAmbientLedCount",
    source,
)
assert re.search(
    r"ambientFrame\[ambientPixelTestIndex\]\s*=\s*ambientPixelTestColor",
    source,
)
assert "AMBIENT_PIXEL" in source

print("PASS: key LEDs follow 1..6; U7-U14 use 8 pixels and U14 is addressable")
