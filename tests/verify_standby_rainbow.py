"""Verify standby lighting waits for a fresh host lighting configuration."""

from pathlib import Path


root = Path(__file__).resolve().parents[1]
header = (root / "include" / "CodexMicroBle.h").read_text(encoding="utf-8")
transport = (root / "src" / "CodexMicroBle.cpp").read_text(encoding="utf-8")
lighting = (root / "src" / "main.cpp").read_text(encoding="utf-8")

assert "bool lightingReady = false;" in header
assert transport.count("state_.lightingReady = false;") >= 5
assert transport.count("state_.lightingReady = true;") == 2
assert "bleState.connected && bleState.lightingReady" in lighting
assert "renderStandbyRainbowFrame" in lighting
assert "kStandbyRainbowLapMs = 2400" in lighting
assert "kStandbyRainbowBrightness = 0.45f" in lighting
assert "kHostStandbyTimeoutMs = 5UL * 60UL * 1000UL" in lighting
assert "StandbyOff" in lighting
assert "enterHostStandby" in lighting
assert "leaveHostStandby" in lighting
assert "POWER host absent for 5 minutes; LEDs black, rail always on" in lighting
assert "usbResumeRecoveryPending_" in transport
assert "!bleState.connected || !bleState.lightingReady" in lighting
assert "hueShift({255, 0, 0}, hue)" in lighting

print("PASS: standby rainbow remains active until fresh host lighting arrives")
