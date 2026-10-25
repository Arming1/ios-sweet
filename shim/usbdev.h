#pragma once
#include <stdint.h>

/*
 * U2 (-DEXPERIMENT=16): USB 2.0 high-speed vendor-class device on venus's
 * DWC_usb31 (usbdev.c). Windows binds WinUSB through the MS OS 2.0 descriptors.
 * Everything is polled: nothing here blocks except short register waits.
 */

/*
 * Start the device (MMU must be on, see usbdev_dma_window). rx is called from
 * usbdev_poll() for each completed bulk OUT transfer (data, len bytes, valid
 * only during the call). Returns 0, or <0 on failure: -2 GCC power/clocks off,
 * -3 not a DWC_usb31 v1.10a, -4 soft reset stuck, -5 controller not running,
 * -6 endpoint 0 setup failed, -1 already started. On failure after the
 * registers were touched, usbdev_stop restores the ABL state.
 */
int usbdev_init(void (*rx)(const uint8_t *, uint32_t));

/* Process every pending event (non-blocking). Call it often. */
void usbdev_poll(void);

/* 1 when SET_CONFIGURATION(1) is active (bulk OUT armed). */
int usbdev_configured(void);

/*
 * bit0 bus reset seen, bit1 connect done, bit2 high speed, bit3 addressed,
 * bit4 configured.
 */
uint32_t usbdev_state_bits(void);

/*
 * End transfers, clear RUN/STOP, wait for DEVCTRLHLT, drain the event buffer,
 * and restore the ABL values of DCFG, DEVTEN, DALEPENA, GEVNTADR/SIZ and the
 * QSCRATCH PHY overrides. Afterwards the controller is in device mode and
 * halted, as ABL left it. Safe to call when not started.
 */
void usbdev_stop(void);

/*
 * The DMA area (16 KiB, 16 KiB aligned): pass it to mmu_set_nc_window. The
 * bulk OUT buffers are not in it (they stay cacheable, see usbdev.c).
 */
void usbdev_dma_window(uintptr_t *lo, uintptr_t *hi);

/*
 * Counters for the status screen: 0 events, 1 SETUP packets, 2 bulk OUT
 * transfers, 3 failed DEPCMDs, 4 ep0 state, 5 live DSTS (0 when not started).
 */
uint32_t usbdev_stat(unsigned which);
