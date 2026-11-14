@echo off
setlocal
title ESP32-S3 firmware flasher
set "ROOT=%~dp0"
set "PYTHON=C:\Users\14376\AppData\Local\Programs\Python\Python310\python.exe"
set "ESPTOOL=C:\Users\14376\.platformio\packages\tool-esptoolpy\esptool.py"
set "BIN=%ROOT%dist\reCodex-no-joystick-merged.bin"
if not exist "%BIN%" for /f "delims=" %%F in ('dir /b /a-d "%ROOT%dist\*.bin" 2^>nul ^| findstr /i "merged"') do set "BIN=%ROOT%dist\%%F"
set "PORTFILE=%TEMP%\codex_port.txt"
if not exist "%BIN%" (
  echo Firmware not found: "%BIN%"
  pause
  exit /b 1
)
del /q "%PORTFILE%" 2>nul
powershell -NoProfile -Command "[IO.Ports.SerialPort]::GetPortNames() -join ','" > "%PORTFILE%"
echo First, unplug the USB-TTL flasher if it is connected.
set /p BEFORE=<"%PORTFILE%"
del /q "%PORTFILE%" 2>nul
echo NOW plug in the USB-TTL flasher.
echo Waiting for a new COM port for up to 30 seconds...
powershell -NoProfile -Command "Start-Sleep -Seconds 2"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%detect_new_port.ps1" -Before "%BEFORE%" -OutFile "%PORTFILE%"
set /p PORT=<"%PORTFILE%"
del /q "%PORTFILE%" 2>nul
if "%PORT%"=="" (
  echo No new COM port found.
  pause
  exit /b 1
)
echo Using %PORT%
"%PYTHON%" "%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 115200 chip_id
if errorlevel 1 (
  echo Chip detection failed. Enter download mode first.
  pause
  exit /b 1
)
choice /m "Erase and flash firmware"
if errorlevel 2 exit /b 0
"%PYTHON%" "%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 115200 erase_flash
"%PYTHON%" "%ESPTOOL%" --chip esp32s3 --port %PORT% --baud 115200 write_flash --flash_size detect 0x0 "%BIN%"
if errorlevel 1 (
  echo Flash failed.
  pause
  exit /b 1
)
echo Flash completed. Disconnect GPIO0 from GND and reset the board.
pause
