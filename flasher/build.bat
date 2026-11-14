@echo off
setlocal
cd /d "%~dp0.."
python -m PyInstaller --noconfirm --clean --onefile --windowed --name reCodex-Flasher --distpath . --workpath .flasher-build --specpath flasher --collect-all esptool --copy-metadata esptool flasher\flasher.py
exit /b %errorlevel%
