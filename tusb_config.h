// tusb_config.h
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------+
// Board Specific Configuration
//--------------------------------------------------------------------+
// PIO-USB host lives on RHPORT 1, NOT the native RHPORT 0.
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT      1
#endif

// Full-speed only (Pico-PIO-USB is FS only)
#ifndef BOARD_TUH_MAX_SPEED
#define BOARD_TUH_MAX_SPEED   OPT_MODE_FULL_SPEED
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU          OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

//--------------------------------------------------------------------
// HOST STACK ENABLED (device stack fully off — this board never
// enumerates to a PC, it only hosts the keyboard controller)
//--------------------------------------------------------------------
#define CFG_TUH_ENABLED       1
#define CFG_TUD_ENABLED       0

// Tell TinyUSB the RHPORT1 host driver is Pico-PIO-USB
#define CFG_TUH_RPI_PIO_USB   1

#define CFG_TUH_MAX_SPEED     BOARD_TUH_MAX_SPEED

//--------------------------------------------------------------------
// MEMORY SECTION
//--------------------------------------------------------------------
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN    __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------
// HOST CONFIGURATION
//--------------------------------------------------------------------
#ifndef CFG_TUH_DEVICE_MAX
#define CFG_TUH_DEVICE_MAX    1   // just the keyboard
#endif

#ifndef CFG_TUH_ENDPOINT0_SIZE
#define CFG_TUH_ENDPOINT0_SIZE 64
#endif

//------------- CLASS DRIVERS -------------//
#define CFG_TUH_HID           4   // multiple HID interfaces on one composite keyboard
#define CFG_TUH_CDC           0
#define CFG_TUH_MSC           0
#define CFG_TUH_MIDI          0
#define CFG_TUH_VENDOR        0

// HID buffer size (for input reports)
#ifndef CFG_TUH_HID_EP_BUFSIZE
#define CFG_TUH_HID_EP_BUFSIZE 64
#endif

#define CFG_TUH_HID_KEYBOARD  1

#ifdef __cplusplus
 }
#endif

#endif
