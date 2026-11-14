"""Exercise the packaged GUI and worker without opening a hardware port."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
exe = root / "reCodex-Flasher.exe"
flags = subprocess.CREATE_NO_WINDOW
subprocess.run([str(exe), "--smoke-test"], check=True, timeout=60, creationflags=flags)
with tempfile.TemporaryDirectory(prefix="recodex-exe-test-") as directory:
    log = Path(directory) / "worker.log"
    for name in ["reCodex-no-joystick-app.bin", "reCodex-no-joystick-merged.bin"]:
        subprocess.run([str(exe), "--worker", str(log), "--chip", "esp32s3",
                        "image-info", str(root / "dist" / name)],
                       check=True, timeout=60, creationflags=flags)
        output = log.read_text(encoding="utf-8")
        assert "ESP32-S3" in output and "(valid)" in output, output
    invalid = Path(directory) / "invalid.bin"
    invalid.write_bytes(b"invalid firmware")
    result = subprocess.run([str(exe), "--worker", str(log), "--chip", "esp32s3",
                             "image-info", str(invalid)], timeout=60, creationflags=flags)
    assert result.returncode != 0, "Worker must propagate failures to the GUI"
    assert log.stat().st_size > 0
print("PASS: packaged GUI, both images, bundled esptool, and error propagation")
