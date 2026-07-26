# RP2040 USB HID host to nRF52840 SPI bridge

This firmware runs on the RP2040 board inside the wireless keyboard build.
It reads the keyboard as a USB HID host through PIO-USB, applies local keyboard
filters, then forwards the 8-byte boot keyboard report to the nRF52840
transmitter over SPI.

## Active wiring

### USB keyboard / converter board to RP2040

| USB side | RP2040 | Purpose |
| --- | --- | --- |
| D+ | GP4 | PIO-USB host D+ |
| D- | GP5 | PIO-USB host D- |
| GND | GND | Common reference for D+/D- |

### RP2040 to nRF52840 transmitter

| RP2040 | nRF52840 transmitter | Purpose |
| --- | --- | --- |
| GP6 | P0.17 | SPI SCK |
| GP7 | P0.20 | SPI MOSI, RP2040 to nRF |
| GP8 | P0.08 | SPI MISO, unused by the current write-only protocol |
| GP9 | P0.22 | SPI CSN, driven low for each 8-byte frame |
| VSYS | VCC / RAW / VIN | nRF power from the Pico board |
| GND | GND | Common ground |

Keep all data-side grounds common: USB converter GND to RP2040 GND, and
RP2040 GND to nRF52840 GND.

## Runtime behavior

- USB host D+ is `GP4`; PIO-USB uses `GP5` as D-.
- SPI is `spi0`, 8 MHz, mode 0, MSB first.
- The forwarded report is the standard 8-byte boot keyboard report.
- A/D and W/S use last-input-wins null movement filtering.
- Per-report UART logging is disabled by default because it breaks 1 kHz input.
- Long key holds are forwarded as state changes instead of being cut after the
  first few repeats.

## Firmware

The build copies the current UF2 to:

```text
firmware/WirelessKeyboard.uf2
```

For a normal board, flash either the build output or the committed UF2 artifact.

For a damaged/debug board whose external QSPI flash cannot erase or program,
`WirelessKeyboardRam` is a RAM-only target. It is diagnostic and does not survive
reset or power loss.
