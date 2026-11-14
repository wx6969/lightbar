"""Check the final physical 1..12 key order."""
from pathlib import Path
import re

source = (Path(__file__).resolve().parents[1] / "src" / "main.cpp").read_text(
    encoding="utf-8"
)
raw_table = re.search(r"kRawBitBySwitch\[12\] = \{([^}]+)\}", source).group(1)
raw_bits = [int(value) for value in re.findall(r"\d+", raw_table)]
binding_table = re.search(
    r"kKeyBindings\[12\] = \{(.*?)\n\};", source, re.S
).group(1)
bindings = re.findall(r'\{"([A-Z0-9]+)",\s*(-?\d+)\}', binding_table)

expected_raw_bits = [4, 5, 6, 7, 0, 1, 2, 3, 8, 9, 10, 11]
expected_keys = [f"AG{i:02}" for i in range(6)] + [
    "ACT06", "ACT07", "ACT08", "ACT09", "ACT10", "ACT12"
]

assert len(raw_bits) == len(set(raw_bits)) == len(bindings) == 12
assert raw_bits == expected_raw_bits
for switch_index, raw_bit in enumerate(expected_raw_bits):
    # Emulate the active-low shift-register input for press and release.
    for pressed in (True, False):
        raw = 0xFFFF ^ (1 << raw_bit) if pressed else 0xFFFF
        active = [i for i, bit in enumerate(raw_bits) if not raw & (1 << bit)]
        assert active == ([switch_index] if pressed else []), (switch_index + 1, active)
    binding, agent = bindings[switch_index]
    assert binding == expected_keys[switch_index], (switch_index + 1, binding)
    assert int(agent) == (switch_index if switch_index < 6 else -1)

assert re.search(r"kAgent0SwitchMask\s*=\s*1U\s*<<\s*0\s*;", source)
print("PASS: final 1..12 key layout, release masks, agent IDs, and Agent 0 chord")
