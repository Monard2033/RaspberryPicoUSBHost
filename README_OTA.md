# Wireless Keyboard OTA Update System — Raport Tehnic & Ghid de Utilizare

Acest document descrie arhitectura, modificările realizate, uneltele de compilare/flashing și planul de dezvoltare viitor (**TODO**) pentru sistemul de actualizare firmware prin aer (**Wireless OTA**) dintre Dongle-ul USB (Nordic nRF52840 Receiver), Modulul Radio Tastatură (Nordic nRF52840 Transmitter) și Microcontrolerul Principal (Raspberry Pi RP2040).

---

## 1. Arhitectura Lanțului de Transmisie OTA

```
+------------------+         USB HID         +------------------------+
|     PC Host      | <---------------------> |   Dongle (Receiver)    |
| (flash_ota tool) |    (Feature Reports)    |  Nordic nRF52840 (NCS) |
+------------------+                         +------------------------+
                                                         ^
                                                         | 2.4 GHz ESB Radio
                                                         | (Enhanced ShockBurst)
                                                         v
+------------------+         SPI Bus         +------------------------+
| Tastatură RP2040 | <---------------------> | Transmitter Keyboard   |
| (Pico SDK C/C++) |     (8 MHz Master)      |  Nordic nRF52840 (NCS) |
+------------------+                         +------------------------+
```

---

## 2. Modificări Implementate & Rezolvări Critice

### A. Dongle USB Receiver (`Receiver/src/main.c`)
1. **Eliminarea blocării radio la transfer intens:**
   - Anterior, `receiver_esb_event_handler` apela `esb_write_payload()` pe fiecare pachet radio de intrare. În stack-ul Nordic (`nrf/subsys/esb/esb.c`), această funcție dezactiva și reactiva întreruperile radio hardware (`irq_disable`/`irq_enable`), corupând mașina de stări ESB.
   - S-a eliminat apelul redundant din handlerul RX; `esb_write_payload()` este apelat acum **exclusiv o singură dată per comandă primită de la Host USB**.
2. **Prevenirea supraîncărcării bufferului de jurnalizare Zephyr:**
   - S-au eliminat mesajele `LOG_INF` din buclele de mare frecvență ale transferului DFU, prevenind blocarea cozii de mesaje din RTOS.

### B. Tastatură RP2040 (`WirelessKeyboard.c`)
1. **Rezolvarea căderii în modul BOOTSEL (Swap curat din SRAM):**
   - Anterior, rutina de swap ștergea Slotul 0 din Flash în timp ce încerca să citească din Flash (conflict XIP cache).
   - S-a adăugat bufferul static `dfu_swap_sram_buffer[64 * 1024]`. La primirea comenzii `ACTIVATE`, întregul binar de 60 KB este copiat integral în SRAM, nucleul Core 1 este oprit, Slotul 0 este șters și rescris atomic din SRAM, iar RP2040 execută **Soft Reboot direct în noua versiune de firmware** (`status = DFU_STATUS_BOOT_OK`).
2. **Trezirea instantanee a nucleului principal Core 0 (`__sev`):**
   - La finalizarea `ACTIVATE`, s-a adăugat instrucțiunea de eveniment `__sev()`. Core 0 iese instantaneu din `__wfe()`, stinge luminile și repornește tastatura în câteva milisecunde, fără a mai fi necesară deconectarea Dongle-ului din PC.
3. **Protecția împotriva somnului radio în timpul DFU:**
   - Adăugat `if (dfu_session_active) return;` în `radio_power_task()` și `battery_task()`, blocând temporizatoarele de inactivitate (5 minute) pe toată durata transferului.
4. **Interogare SPI la viteză ridicată (150 µs):**
   - Comutat interogarea DFU de la 1 ms (`board_millis`) la 150 microsecunde (`SPI_MIN_GUARD_US` prin `time_us_32()`), păstrând neschimbată și complet protejată funcționarea rapoartelor de taste HID/NKRO.

### C. Unelte Host PC (`tools/`)
1. **`tools/flash_ota.py`:**
   - Suport nativ Windows 64-bit prin `ctypes` (`SetupDi*` APIs, structuri cu aliniere strictă).
   - Setare precizie temporizator sistem la 1 ms (`timeBeginPeriod(1)`).
2. **`tools/flash_ota.cpp` & `tools/flash_ota.exe`:**
   - Flasher nativ Win32 C++ de mare viteză.
3. **`tools/probe_ota_link.exe`:**
   - Utilitar rapid pentru diagnosticarea și verificarea stării de funcționare a conexiunii radio.

---

## 3. Ghid Rapid de Comenzi & Utilitare

### A. Compilare Firmware RP2040 (Tastatură)
Deschide PowerShell în folderul `c:\Users\Monard\Raspberry\WirelessKeyboard`:
```powershell
$ninja = "$env:USERPROFILE\.pico-sdk\ninja\v1.13.2\ninja.exe"
& $ninja -C build WirelessKeyboard
```
*Fișiere generate:*
- `firmware/WirelessKeyboard.uf2` (pentru încărcare prin cablu / BOOTSEL)
- `firmware/WirelessKeyboard_OTA.wkota` (pachetul criptat/formatat pentru update prin aer)

### B. Compilare Firmware Receiver Dongle (nRF52840)
Deschide PowerShell în folderul `c:\ncs\v3.4.0\myproject\Receiver`:
```powershell
west build -b nrf52840dongle_nrf52840
```
*Fișier generat:* `build/zephyr/zephyr.hex`

### C. Verificare Stare Conexiune Radio (Ping / Probe)
```powershell
& "C:\Users\Monard\Raspberry\WirelessKeyboard\tools\probe_ota_link.exe" 5
```

### D. Rulare Actualizare Wireless OTA
```powershell
# Varianta Python:
& "$env:USERPROFILE\.pico-sdk\python\3.13.7\python.exe" tools/flash_ota.py firmware/WirelessKeyboard_OTA.wkota

# Varianta C++ nativă:
& "C:\Users\Monard\Raspberry\WirelessKeyboard\tools\flash_ota.exe" "firmware\WirelessKeyboard_OTA.wkota"
```

---

## 4. Plan de Dezvoltare Viitor (TODO)

- [ ] **1. Streaming continuu la nivel de Pagină Flash (256 Bytes):**
  - Adăugare coadă FIFO (`K_MSGQ_DEFINE`) pe Receiver și pe Transmitter pentru a transmite pachetele în flux continuu fără oprire la fiecare 6 bytes.
  - Trimiterea confirmării de la RP2040 o singură dată la fiecare pagină (256 bytes) $\implies$ reducerea numărului de opriri de la 9.920 la 232.
  - **Obiectiv:** Reducerea timpului de transfer la **sub 3–5 secunde**.
- [ ] **2. Animație RGB LED în timpul Flash-ului OTA:**
  - Adăugarea unui efect vizual (de exemplu respirație albastră/pulsare violet pe taste) când `dfu_session_active == true`, urmat de flash verde la confirmarea CRC.
- [ ] **3. Integrare GUI / CLI unificată:**
  - Împachetarea flasherului într-o mică interfață cu bară de progres netedă și auto-detecție a dongle-ului conectat.
