#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ESP32-C6 TRM Rev 1.2: USB-Serial-JTAG and Power Clock Reset (PCR) registers */
#define PCR_USB_CONF_REG         ((volatile uint32_t *)0x6009608C)
#define USB_EP1_DATA_REG         ((volatile uint32_t *)0x6000F000)
#define USB_EP1_CONF_REG         ((volatile uint32_t *)0x6000F004)
#define USB_CONF0_REG            ((volatile uint32_t *)0x6000F018)
#define USB_CHIP_RST_REG         ((volatile uint32_t *)0x6000F04C)
#define USB_CONFIG_UPDATE_REG    ((volatile uint32_t *)0x6000F050)

/* Clock gating and reset controls */
#define PCR_USB_CLK_EN           (1UL << 0)
#define PCR_USB_RST_EN           (1UL << 1)

/* USB_CONF0 hardware pad configuration */
#define USB_PAD_ENABLE           (1UL << 2)

/* EP1 hardware status flags */
#define USB_WR_DONE              (1UL << 0)
#define USB_SERIAL_IN_FREE       (1UL << 1)
#define USB_SERIAL_OUT_AVAIL     (1UL << 2)

/* ESP32-C6 TRM Rev 1.2, Ch. 28.5: Mask hardware resets triggered by host DTR/RTS transitions */
#define USB_UART_CHIP_RST_DIS    (1UL << 2)
#define USB_SERIAL_CONFIG_UPDATE (1UL << 0)

extern volatile uint32_t g_hw_usb_lock;

/**
 * @brief Telemetry tap callback function pointer.
 * Used to mirror console output bytes to secondary streaming sinks (e.g. c6wsh web socket pipe).
 */
typedef void (*hw_usb_tap_fn_t)(const uint8_t *data, size_t len);

extern volatile hw_usb_tap_fn_t g_hw_usb_tap;

/**
 * @brief Forward declaration of Management Engine watchdog feeding routine.
 * Allows bare-metal USB drivers to service the LP-Core heartbeat during long FIFO stalls.
 */
void management_engine_pet_watchdog(void);

/**
 * @brief Attaches or detaches an active console telemetry streaming hook.
 *
 * @param tap Destination callback receiving byte duplicates, or NULL to disarm.
 */
static inline void hw_usb_set_tap(hw_usb_tap_fn_t tap)
{
    g_hw_usb_tap = tap;
}

/**
 * @brief Acquires atomic spinlock using GCC atomic intrinsics with safety timeout break.
 */
static inline void hw_usb_lock(void)
{
    uint32_t spin = 0;
    while (__atomic_test_and_set(&g_hw_usb_lock, __ATOMIC_ACQUIRE)) {
        if (++spin > 1000000) {
            g_hw_usb_lock = 0;
            break;
        }
    }
}

/**
 * @brief Releases atomic interface spinlock.
 */
static inline void hw_usb_unlock(void)
{
    __atomic_clear(&g_hw_usb_lock, __ATOMIC_RELEASE);
}

/**
 * @brief Initializes native hardware USB-Serial-JTAG PHY and disables auto-reset logic.
 */
static inline void hw_usb_init(void)
{
    *PCR_USB_CONF_REG |= PCR_USB_CLK_EN;
    *PCR_USB_CONF_REG &= ~PCR_USB_RST_EN;
    *USB_CONF0_REG |= USB_PAD_ENABLE;
    *USB_CHIP_RST_REG |= USB_UART_CHIP_RST_DIS;
    *USB_CONFIG_UPDATE_REG |= USB_SERIAL_CONFIG_UPDATE;
}

/**
 * @brief Transmits a single byte to the USB EP1 IN FIFO.
 */
static inline void hw_usb_putc(uint8_t c)
{
    *USB_EP1_DATA_REG = (uint32_t)c;
}

/**
 * @brief Commits data in EP1 FIFO for host packet transmission.
 */
static inline void hw_usb_flush(void)
{
    *USB_EP1_CONF_REG = USB_WR_DONE;
}

/**
 * @brief Polls whether inbound host data is available in the OUT FIFO.
 */
static inline bool hw_usb_has_data(void)
{
    return (*USB_EP1_CONF_REG & USB_SERIAL_OUT_AVAIL) != 0;
}

/**
 * @brief Reads a single byte from the USB EP1 OUT FIFO.
 */
static inline char hw_usb_getc(void)
{
    return (char)(*USB_EP1_DATA_REG & 0xFF);
}

/**
 * @brief Transmits a null-terminated string over native USB-Serial-JTAG.
 * Segments into 64-byte packets and pets LP watchdog during FIFO wait loops.
 * Dispatches to telemetry tap outside FIFO spinlock to eliminate lock inversion deadlocks.
 *
 * @param str Null-terminated ASCII string.
 */
static inline void hw_usb_print(const char *str)
{
    if (!str) {
        return;
    }

    hw_usb_tap_fn_t tap = g_hw_usb_tap;
    if (tap) {
        size_t slen = strlen(str);
        if (slen > 0) {
            tap((const uint8_t *)str, slen);
        }
    }

    /* If USB host terminal is not actively draining FIFO, drop hardware transmission */
    if (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE)) {
        uint32_t quick_poll = 500;
        while (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE) && --quick_poll) {
            __asm__ volatile("nop");
        }
        if (quick_poll == 0) {
            return;
        }
    }

    hw_usb_lock();
    uint32_t chunk_len = 0;

    while (*str) {
        if (chunk_len >= 64) {
            hw_usb_flush();
            chunk_len = 0;

            /* Break out if USB host terminal is not actively reading endpoint */
            uint32_t timeout = 50000;
            while (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE) && --timeout) {
                __asm__ volatile("nop");
            }
            if (timeout == 0) {
                hw_usb_unlock();
                return;
            }
        }

        if (*str == '\n') {
            hw_usb_putc('\r');
            chunk_len++;
        }

        hw_usb_putc((uint8_t)*str);
        chunk_len++;
        str++;
    }

    if (chunk_len > 0) {
        hw_usb_flush();
        uint32_t timeout = 50000;
        while (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE) && --timeout) {
            __asm__ volatile("nop");
        }
    }

    hw_usb_unlock();
}

/**
 * @brief Writes raw byte buffer directly to USB endpoint FIFO.
 * Invoked by Newlib POSIX _write() implementation.
 *
 * @param src Pointer to source data buffer.
 * @param len Number of bytes to transmit.
 */
static inline void hw_usb_write_bytes(const uint8_t *src, uint32_t len)
{
    if (!src || len == 0) {
        return;
    }

    hw_usb_tap_fn_t tap = g_hw_usb_tap;
    if (tap) {
        tap(src, (size_t)len);
    }

    hw_usb_lock();
    uint32_t chunk = 0;

    uint32_t timeout = 50000;
    while (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE) && --timeout) {
        __asm__ volatile("nop");
    }
    if (timeout == 0) {
        hw_usb_unlock();
        return;
    }

    for (uint32_t i = 0; i < len; i++) {
        if (chunk >= 64) {
            hw_usb_flush();
            chunk = 0;

            timeout = 50000;
            while (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE) && --timeout) {
                __asm__ volatile("nop");
            }
            if (timeout == 0) {
                hw_usb_unlock();
                return;
            }
        }
        hw_usb_putc(src[i]);
        chunk++;
    }

    if (chunk > 0) {
        hw_usb_flush();
        timeout = 50000;
        while (!(*USB_EP1_CONF_REG & USB_SERIAL_IN_FREE) && --timeout) {
            __asm__ volatile("nop");
        }
    }

    hw_usb_unlock();
}

/**
 * @brief Synchronous formatted print routing to bare-metal USB registers.
 *
 * @param fmt Printf-style format string.
 */
static inline void dbg_usb_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        hw_usb_print(buf);
    }
}
