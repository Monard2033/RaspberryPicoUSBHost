# Build script for the OTA flasher executables (protocol v2).
# Usage: powershell -File tools\build_ota_flasher.ps1
# Requires: the bundled pico-sdk Python with PyInstaller 6.x.
$py = "$env:USERPROFILE\.pico-sdk\python\3.13.7\python.exe"
$tools = $PSScriptRoot
Push-Location $tools
& $py -m PyInstaller --onefile --name flash_ota_cmd flash_ota_cmd.py --distpath . --workpath _pyi_build --specpath .
& $py -m PyInstaller --onefile --windowed --name Flash_Ota flash_ota_gui.py --distpath . --workpath _pyi_build --specpath .
Remove-Item _pyi_build -Recurse -Force -ErrorAction SilentlyContinue
Pop-Location
