# lightbar

reCodex 是运行在 ESP32-S3 上的 USB HID 输入/灯光设备固件。本仓库是其中的
**无摇杆（no-joystick）变体**：设备作为纯 USB 有线 HID 键盘使用，BLE、摇杆、
电池与低功耗逻辑均在编译期关闭，避免无线传输干扰 USB 枚举。

## 目标硬件

- MCU：ESP32-S3（16 MB Flash，OPI PSRAM）
- 框架：Arduino（PlatformIO，espressif32@6.13.0）
- USB 身份：VID `303A`，PID `8360`
- 设备形态：总线供电（bus-powered），HID 键盘

## 功能摘要

- 12 个物理按键映射为宿主按键 1–12
- 6 个按键指示灯 + 8 个氛围灯（可寻址 RGB，RMT 驱动）
- 旋转编码器（方向已修正）
- HID 中断 IN 端点 `0x81`，宿主输出报告走控制端点 EP0 的 `SET_REPORT`
- USB 挂起/唤醒后自动重新枚举，修复 PC 睡眠唤醒后的失效句柄
- 待机时氛围灯显示低亮度流动彩虹，连续 5 分钟无宿主数据后进入暗态

## 目录结构

```
.
├── platformio.ini        # 主固件构建配置
├── include/              # 头文件
│   ├── CodexMicroBle.h
│   ├── ControlOnlyUsbHid.h
│   └── HidOtaUpdate.h
├── src/                  # 固件源码
│   ├── main.cpp
│   ├── CodexMicroBle.cpp
│   ├── ControlOnlyUsbHid.cpp
│   └── HidOtaUpdate.cpp
├── flasher/              # PC 端烧录工具（Python GUI + 内置 esptool）
├── tests/                # 校验脚本
├── scripts/              # 一键烧录辅助脚本
└── diagnostics/          # USB 分阶段诊断固件（DIAG_STAGE 1–4）
```

## 构建

依赖 [PlatformIO](https://platformio.org/)。在仓库根目录执行：

```sh
pio run
```

构建产物输出到 `.pio/`，已通过 `.gitignore` 忽略。

## 烧录

任选其一：

1. **图形界面**：运行 `flasher/flasher.py`（Python 3.10，`pyserial`、`esptool`），
   或按 `flasher/build.bat` 用 PyInstaller 重新打包出 EXE。
2. **一键脚本**：`scripts/一键烧录-无摇杆.bat` / `scripts/flash_no_joystick.bat`
   （会先检测新插入的串口，再擦除并写入完整镜像到 `0x0`）。
3. **手动 esptool**：
   ```sh
   esptool.py --chip esp32s3 --port COMx --baud 115200 write_flash 0x0 merged.bin
   ```

烧录说明详见 `flasher/README.md`。烧录时需要把 GPIO0 接地进入下载模式，
完成后断开并复位。

## 测试

`tests/` 下是构建后用于回归的校验脚本，覆盖按键顺序、LED 链、打包后的烧录工具、
待机彩虹、USB 控制传输等。可用 Python 3 运行：

```sh
python tests/verify_key_order.py
```

## 说明

- 本仓库**只包含源码**，不包含预编译产物（`.bin`、`.exe`）以及
  `.pio/`、`.flasher-build/` 等构建缓存与诊断日志，请按上文自行构建。
- `src/main.cpp` 与 `diagnostics/src/main.cpp` 为两套独立固件，构建时请以各自
  目录下的 `platformio.ini` 为准。

## 许可证

当前仓库未附带 `LICENSE` 文件。如需开源，请自行选择合适的许可证并添加。
