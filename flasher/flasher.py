"""Windows GUI for the reCodex ESP32-S3 firmware. Also runs bundled esptool."""
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time


def firmware_address(path):
    data = Path(path).read_bytes()
    if not 64 <= len(data) <= 16 * 1024 * 1024:
        raise ValueError("文件大小无效：请选择 ESP32-S3 的 .bin 固件。")
    if data[0] != 0xE9 or int.from_bytes(data[12:14], "little") != 9:
        raise ValueError("文件不是 ESP32-S3 固件，不能烧录。")
    if data[32:36] == bytes.fromhex("3254cdab"):
        if len(data) > 0x640000:
            raise ValueError("应用镜像超过本项目的应用分区大小。")
        return "0x10000"
    if (len(data) > 0x10040 and data[0x10000] == 0xE9
            and data[0x1000C:0x1000E] == b"\x09\x00"
            and data[0x10020:0x10024] == bytes.fromhex("3254cdab")
            and data[0x8000:0x8002] == b"\xaa\x50"):
        return "0x0"
    raise ValueError("未识别的布局。请选择本项目完整镜像或应用镜像；不支持单独的引导程序。")


def worker(logfile, args):
    # Windowed executables have no stdout. Give esptool a real UTF-8 stream.
    with open(logfile, "w", encoding="utf-8", buffering=1) as output:
        sys.stdout = sys.stderr = output
        os.environ["NO_COLOR"] = "1"
        try:
            import esptool
            esptool.main(args)
        except SystemExit as exc:
            return exc.code if isinstance(exc.code, int) else 1
        except Exception:
            import traceback
            traceback.print_exc()
            return 1
    return 0


def main():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    from tkinter.scrolledtext import ScrolledText
    from serial.tools import list_ports

    root = tk.Tk()
    root.title("reCodex · ESP32-S3 固件烧录工具")
    root.geometry("860x650")
    root.minsize(760, 580)
    style = ttk.Style()
    style.theme_use("vista")
    style.configure("TLabel", font=("Microsoft YaHei UI", 10))
    style.configure("TButton", font=("Microsoft YaHei UI", 10), padding=6)
    frame = ttk.Frame(root, padding=20)
    frame.pack(fill="both", expand=True)
    frame.columnconfigure(1, weight=1)
    frame.rowconfigure(8, weight=1)
    ttk.Label(frame, text="ESP32-S3 固件烧录", font=("Microsoft YaHei UI", 18, "bold")).grid(
        row=0, column=0, columnspan=3, sticky="w", pady=(0, 14))
    base = Path(sys.executable).parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parents[1]
    candidates = [base / "dist" / "reCodex-no-joystick-merged.bin",
                  base / "reCodex-no-joystick-merged.bin"]
    firmware = tk.StringVar(value=str(next((p for p in candidates if p.exists()), "")))
    port = tk.StringVar()
    baud = tk.StringVar(value="115200")
    address = tk.StringVar(value="选择文件后自动识别烧录地址")
    status = tk.StringVar(value="正在扫描串口…")
    events = queue.Queue()
    state = {"busy": False, "ports": {}, "seen": set(), "scanned": False}

    def check_file():
        try:
            offset = firmware_address(firmware.get())
            address.set(("完整固件" if offset == "0x0" else "仅更新应用（需已烧录完整固件）")
                        + " · 地址 " + offset)
            return offset
        except (OSError, ValueError) as exc:
            address.set(str(exc))
            return None

    def browse():
        path = filedialog.askopenfilename(title="选择 ESP32-S3 固件", filetypes=[("固件文件", "*.bin")])
        if path:
            firmware.set(path)
            check_file()

    ttk.Label(frame, text="烧录文件").grid(row=1, column=0, sticky="w", padx=(0, 12))
    file_entry = ttk.Entry(frame, textvariable=firmware, state="readonly")
    file_entry.grid(row=1, column=1, sticky="ew")
    browse_button = ttk.Button(frame, text="选择文件…", command=browse)
    browse_button.grid(row=1, column=2, padx=(10, 0))
    ttk.Label(frame, textvariable=address).grid(row=2, column=1, columnspan=2, sticky="w", pady=(4, 14))
    ttk.Label(frame, text="烧录器串口").grid(row=3, column=0, sticky="w")
    ports_box = ttk.Combobox(frame, textvariable=port, state="readonly")
    ports_box.grid(row=3, column=1, columnspan=2, sticky="ew")
    ttk.Label(frame, text="自动刷新；优先选中新插入的 USB 串口。多个设备时可手动选择。",
              foreground="#555555").grid(row=4, column=1, columnspan=2, sticky="w", pady=(4, 10))
    ttk.Label(frame, text="烧录速度").grid(row=5, column=0, sticky="w")
    baud_box = ttk.Combobox(frame, textvariable=baud, values=["115200", "460800", "921600"], state="readonly", width=14)
    baud_box.grid(row=5, column=1, sticky="w")
    actions = ttk.Frame(frame)
    actions.grid(row=6, column=0, columnspan=3, sticky="ew", pady=14)
    progress = ttk.Progressbar(frame, mode="indeterminate")
    progress.grid(row=7, column=0, columnspan=3, sticky="ew", pady=(0, 10))
    log = ScrolledText(frame, wrap="word", font=("Consolas", 10), state="disabled", height=16)
    log.grid(row=8, column=0, columnspan=3, sticky="nsew")
    ttk.Label(frame, textvariable=status, wraplength=790).grid(row=9, column=0, columnspan=3, sticky="w", pady=(10, 0))
    ttk.Label(frame, text="连接失败：按住 BOOT 再按 RESET，松开 BOOT 后重试；使用 USB-TTL 时需共地、TX/RX 交叉。",
              wraplength=790, foreground="#555555").grid(row=10, column=0, columnspan=3, sticky="w", pady=(8, 0))

    def append(text):
        text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text).replace("\r", "\n")
        log.configure(state="normal")
        log.insert("end", text)
        log.see("end")
        log.configure(state="disabled")

    def scan():
        if not state["busy"]:
            try:
                devices = sorted(list_ports.comports(), key=lambda p: (p.vid is None, p.device))
                current_device = state["ports"].get(port.get())
                mapping = {f"{p.device} — {p.description}": p.device for p in devices}
                new = {p.device for p in devices} - state["seen"]
                selected = next((label for label, device in mapping.items() if device == current_device), "")
                if state["scanned"] and len(new) == 1:
                    selected = next(label for label, device in mapping.items() if device in new)
                if not selected and mapping:
                    selected = next(iter(mapping))
                ports_box["values"] = list(mapping)
                port.set(selected)
                if mapping != state["ports"] or not state["scanned"]:
                    status.set(f"发现 {len(mapping)} 个串口；可点击检测芯片或开始烧录。" if mapping else "未发现串口，请插入烧录器并确认驱动已安装。")
                state.update(ports=mapping, seen={p.device for p in devices}, scanned=True)
            except Exception as exc:
                status.set("串口扫描失败：" + str(exc))
        root.after(1000, scan)

    def run_command(args):
        with tempfile.TemporaryDirectory(prefix="recodex-flash-") as temporary:
            logfile = Path(temporary) / "output.txt"
            logfile.touch()
            prefix = [sys.executable] if getattr(sys, "frozen", False) else [sys.executable, str(Path(__file__).resolve())]
            process = subprocess.Popen(prefix + ["--worker", str(logfile)] + args,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            with logfile.open(encoding="utf-8", errors="replace") as output:
                while process.poll() is None:
                    chunk = output.read()
                    if chunk:
                        events.put(("log", chunk))
                    time.sleep(0.1)
                events.put(("log", output.read()))
            return process.returncode

    def execute(device, speed, path, offset):
        try:
            common = ["--chip", "esp32s3", "--port", device, "--baud", speed, "--connect-attempts", "3"]
            result = run_command(common + ["chip-id"])
            if result == 0 and path:
                events.put(("status", "芯片已识别，正在烧录并校验，请勿拔线…"))
                result = run_command(common + ["write-flash", "--flash-size", "detect", offset, path])
            events.put(("done", (result, bool(path))))
        except Exception as exc:
            events.put(("log", "\n" + str(exc) + "\n"))
            events.put(("done", (1, bool(path))))

    def start(flash):
        if state["busy"]:
            return
        device = state["ports"].get(port.get())
        if not device:
            messagebox.showerror("未选择烧录器", "请插入烧录器，并选择串口。")
            return
        offset = check_file() if flash else None
        if flash and offset is None:
            messagebox.showerror("固件文件无效", address.get())
            return
        state["busy"] = True
        for widget in (browse_button, probe_button, flash_button, ports_box, baud_box):
            widget.configure(state="disabled")
        progress.start(12)
        status.set("正在连接并检测 ESP32-S3…")
        append("\n" + "─" * 45 + "\n" + device + (f" · {firmware.get()} · {offset}" if flash else " · 检测芯片") + "\n")
        threading.Thread(target=execute, args=(device, baud.get(), firmware.get() if flash else None, offset), daemon=True).start()

    probe_button = ttk.Button(actions, text="检测芯片", command=lambda: start(False))
    probe_button.pack(side="left")
    flash_button = ttk.Button(actions, text="开始烧录", command=lambda: start(True))
    flash_button.pack(side="right")

    def pump():
        try:
            while True:
                kind, payload = events.get_nowait()
                if kind == "log":
                    append(payload)
                elif kind == "status":
                    status.set(payload)
                elif kind == "done":
                    code, flashed = payload
                    state["busy"] = False
                    progress.stop()
                    for widget in (browse_button, probe_button, flash_button):
                        widget.configure(state="normal")
                    ports_box.configure(state="readonly")
                    baud_box.configure(state="readonly")
                    if code == 0:
                        message = "烧录及校验成功。若 BOOT / GPIO0 接地，请断开后按 RESET。" if flashed else "已确认 ESP32-S3，芯片信息见日志。"
                    else:
                        message = "操作失败，请查看日志；检查下载模式、接线和串口占用后重试。"
                    status.set(message)
                    append("\n" + message + "\n")
        except queue.Empty:
            pass
        root.after(100, pump)

    def close():
        if state["busy"]:
            messagebox.showinfo("操作进行中", "请等待检测或烧录完成后关闭，避免中断写入。")
        else:
            root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    check_file()
    scan()
    pump()
    if "--smoke-test" in sys.argv:
        root.after(1500, root.destroy)
    root.mainloop()


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--worker":
        sys.exit(worker(sys.argv[2], sys.argv[3:]))
    main()
