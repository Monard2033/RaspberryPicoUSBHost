@echo off
C:\Users\Monard\.pico-sdk\picotool\2.2.0\picotool\picotool.exe reboot -f -u
timeout /t 1
C:\Users\Monard\.pico-sdk\picotool\2.2.0\picotool\picotool.exe load c:/Raspberry/WirelessKeyboard/build/WirelessKeyboard.uf2 -x