# Root-cause checklist: OTA enters BOOTSEL after slot swap

1. Confirm the staged image CRC matched (tool log). If it did, the staging area is fine — the swap path corrupted slot 0.
2. In the linker map, find the VMA of every function called inside the erase/program window. Anything at 0x100xxxxx (XIP flash) is a crash candidate: watchdog_update, printf, assert, logging helpers.
3. Check `dfu_apply_and_reboot` placement: it must be in `.time_critical.*` mapped at 0x2000xxxx (RAM). The `__no_inline_not_in_flash_func` attribute alone is not proof — read the map.
4. Check the SRAM budget: `.bss.dfu_swap_sram_buffer` start + size must stay below `__StackLimit` (0x20040000 default).
5. After fixing, re-verify with a full radio OTA cycle: transfer 100%, staging CRC MATCH, then post-reboot probe (`probe_ota_link.exe 3`) must echo status=0x0C (BOOT_OK) with the new package CRC, and the RPI-RP2 drive must NOT appear.
6. Flash timing budget on W25Q-class flash: 4 KB sector erase ~45 ms, 256 B page program ~1 ms; 128 KB image ~1.2 s total, safely watchdog-free once the watchdog is disabled.
