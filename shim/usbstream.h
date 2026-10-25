#pragma once

/*
 * Live USB stream (usbstream.c): frames from the PC over the bulk OUT pipe of
 * usbdev.c, drawn as they arrive. Wire format and host tool: see usbstream.c
 * and tools/win-usbstream.py --play.
 *
 * Starts the USB device (MMU on, NC DMA window) and waits up to wait_ms for a
 * "SWVS" start message, counted from usbdev_init. Returns 0 when no stream
 * came (the device is stopped and the MMU is off again: the caller boots as
 * usual). Returns 1 when a stream ended with "boot" (SWVE a=1), or the host
 * disconnected for more than 3 s during a stream; the device is stopped and
 * the MMU is off again. A stream that ends with "hold" (SWVE a=0) never
 * returns: the device keeps polling USB and drawing.
 */
int usbstream_run(int wait_ms);
