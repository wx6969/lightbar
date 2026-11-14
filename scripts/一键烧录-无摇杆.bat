@echo off
setlocal
chcp 65001 >nul
title reCodex 无摇杆固件一键烧录

set "ROOT=%~dp0"
set "ESPTOOL=C:\Users\14376\.platformio\packages\tool-esptoolpy\esptool.py"
set "PYTHON=C:\Users\14376\AppData\Local\Programs\Python\Python310\python.exe"
set "FIRMWARE=%ROOT%dist\reCodex-no-joystick-merged.bin"

if not exist "%FIRMWARE%" (
  echo 找不到固件：%FIRMWARE%
  pause
  exit /b 1
)
if not exist "%PYTHON%" (
  echo 找不到 Python：%PYTHON%
  pause
  exit /b 1
)

echo 当前已有串口：
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
set "BEFORE="
for /f "delims=" %%P in ('powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() -join ','"') do set "BEFORE=%%P"
echo.
echo 现在请插入 USB 转串口烧录器，并等待 5 秒...
timeout /t 5 /nobreak >nul
for /f "usebackq delims=" %%P in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%等待新增串口.ps1" -Before "%BEFORE%"`) do set "PORT=%%P"
if "%PORT%"=="" (
  echo 没有检测到新增 COM 端口。
  echo 请确认驱动已安装、烧录器已插入，再重新运行本工具。
  pause
  exit /b 1
)
echo 自动选择端口：%PORT%
if "%PORT%"=="" exit /b 1

echo.
echo 先检测 ESP32-S3 芯片，请确保核心板已进入下载模式。
"%PYTHON%" "%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 115200 chip_id
if errorlevel 1 (
  echo.
  echo 检测失败。请按住 GPIO0 接 GND 后重新上电，再运行本工具。
  pause
  exit /b 1
)

echo.
echo 即将擦除并写入无摇杆固件：
echo %FIRMWARE%
choice /M "确认开始烧录"
if errorlevel 2 exit /b 0

"%PYTHON%" "%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 115200 erase_flash
if errorlevel 1 goto :fail
"%PYTHON%" "%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 115200 write_flash --flash_size detect 0x0 "%FIRMWARE%"
if errorlevel 1 goto :fail

echo.
echo 烧录成功。断开 GPIO0-GND 后按 EN 或重新插拔 USB。
pause
exit /b 0

:fail
echo.
echo 烧录失败。请检查端口、USB线和 GPIO0 下载模式。
pause
exit /b 1
