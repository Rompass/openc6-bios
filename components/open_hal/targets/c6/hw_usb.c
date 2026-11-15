#include "hw_usb.h"

/* Shared atomic spinlock guarding bare-metal USB-Serial-JTAG endpoint FIFOs */
volatile uint32_t g_hw_usb_lock = 0;

/* Active console telemetry tap hook for c6wsh web socket/chunked stream */
volatile hw_usb_tap_fn_t g_hw_usb_tap = NULL;
