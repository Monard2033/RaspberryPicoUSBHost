@echo off
setlocal

set "PICOTOOL=%USERPROFILE%\.pico-sdk\picotool\2.3.0\picotool\picotool.exe"
set "UF2=%~dp0build\WirelessKeyboard.uf2"

if not exist "%PICOTOOL%" (
    echo ERROR: picotool was not found:
    echo   %PICOTOOL%
    exit /b 1
)

if not exist "%UF2%" (
    echo ERROR: firmware was not built:
    echo   %UF2%
    exit /b 1
)

echo Loading:
echo   %UF2%
"%PICOTOOL%" load "%UF2%" -f -x
if not "%ERRORLEVEL%"=="0" (
    echo.
    echo ERROR: RP2040 was not found in BOOTSEL mode.
    echo Connect the TARGET board's native USB while holding BOOTSEL,
    echo then run this file again.
    exit /b 1
)

echo Firmware loaded successfully.
endlocal
