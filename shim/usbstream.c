/*
 * Live USB stream (-DSHIM_USBSTREAM, experiment 17): frames arrive from the PC
 * over the bulk OUT pipe (usbdev.c) and are drawn as they come. No frame is
 * buffered: payload bytes are converted into one line buffer (a source row,
 * each pixel repeated scale times), and each complete row is written to the
 * picture scale times. usbdev.c has re-armed the USB buffer before the
 * callback runs, so the callback consumes its bytes and returns.
 * The picture is Normal non-cacheable: it is only written, never read.
 *
 * Wire format: a byte stream, little-endian. Every message is a 16-byte header
 * (char magic[4]; uint32 a; uint32 b; uint32 len) followed by len payload bytes.
 * A message may be split across USB transfers, and one transfer may hold
 * several messages.
 *   "SWVS" start   a = width << 16 | height, b = 1 RGB565 or 2 PAL8, len 0
 *   "SWVP" palette len 1024: 256 x uint32 0x00RRGGBB (PAL8 only, after SWVS)
 *   "SWVF" frame   a = frame number, b = 0, len = w * h * bpp, raster order
 *   "SWVE" end     a = 0 hold on the last frame, a = 1 boot XNU, len 0
 * A header with an unknown magic, a wrong len or a bad a/b counts one protocol
 * error per run of bad bytes; the parser then drops bytes until a valid header
 * starts again.
 *
 * Picture: the largest integer scale s with w*s <= 1440 and h*s <= 3200,
 * centred. Status line on row 0 (y 24..71), messages on row 1 (y 72..119).
 * A picture that reaches those rows covers them; the next frame overwrites the
 * picture rows again. The picture changes one source row at a time: a row
 * still being received shows the previous frame.
 *
 * Hold (SWVE a=0): the device keeps polling and keeps drawing whatever the host
 * sends next; a new SWVS starts a new stream. Disconnects are not acted upon
 * while holding.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "usbdev.h"
#include "usbstream.h"

#define PIX_PITCH	(FB_STRIDE / 4)		/* framebuffer words per row */
#define HDR_SIZE	16
#define PAL_BYTES	1024
#define FMT_RGB565	1
#define FMT_PAL8	2
#define DISCONNECT_S	3			/* not configured this long mid-stream: boot */

#ifndef USBSTREAM_SIM
/* generic timer; a host build (USBSTREAM_SIM) supplies its own */
static uint64_t ust_now(void)
{
	uint64_t v;
	__asm__ volatile("isb" ::: "memory");
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static uint64_t ust_freq(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f;
}
#endif

enum msg_kind { MSG_NONE, MSG_START, MSG_PAL, MSG_FRAME, MSG_END };
enum pstate { ST_HDR, ST_PAL, ST_FRAME };

/* 64-bit framebuffer stores; may_alias, the picture is also a uint32_t array. */
typedef uint64_t __attribute__((may_alias)) u64_alias;

/* Next source pixel (x, y) of the frame, and the frame geometry. */
struct cursor {
	int x, y, w, scale;
};

/*
 * The source row being filled: w * scale words, each source pixel repeated
 * scale times. Written once per pixel, then copied to the picture scale times
 * when the row is complete (row_out). Cacheable. A frame cut short leaves its
 * unfinished row here; the next frame starts at column 0 and overwrites it.
 */
static uint32_t line[FB_W];

static struct {
	/* parser */
	uint8_t hdr[HDR_SIZE];
	uint32_t hdr_fill;
	enum pstate state;
	uint32_t left;			/* payload bytes still to come (PAL, FRAME) */
	uint32_t acc;			/* palette entry being assembled */
	int resync;			/* inside a run of bad bytes (one error counted) */
	/* stream */
	int active, ended, end_action;
	int fmt, bpp, w, h, scale;
	volatile uint32_t *origin;	/* block of source pixel (0, 0) */
	struct cursor c;
	int has_pend;			/* RGB565: first byte of a pixel that is split */
	uint8_t pend;
	uint32_t cur_frame;
	uint32_t pal[256];		/* ARGB, alpha 0xff */
	/* counters */
	uint32_t frames, last_frame, proto_err;
	uint64_t bytes;			/* stream bytes received */
	uint64_t busy;			/* ticks spent in stream_rx */
} ss;

static uint32_t rd32le(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static enum msg_kind magic_of(const uint8_t *h)
{
	if (h[0] != 'S' || h[1] != 'W' || h[2] != 'V')
		return MSG_NONE;
	switch (h[3]) {
	case 'S': return MSG_START;
	case 'P': return MSG_PAL;
	case 'F': return MSG_FRAME;
	case 'E': return MSG_END;
	default:  return MSG_NONE;
	}
}

/* RGB565 (little-endian pixel) to a8r8g8b8 with bit replication. */
static inline uint32_t rgb565_to_argb(uint32_t v)
{
	uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;

	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	return 0xff000000U | r << 16 | g << 8 | b;
}

/*
 * A complete source row: the line goes to the picture scale times, rows
 * y * scale + k (k = 0 .. scale-1) below the picture origin. Stores are 64 bits
 * (two pixels) where the address allows, after one 32-bit store that aligns it.
 */
static void fb_row(volatile uint32_t *dst, const uint32_t *src, int n)
{
	int i = 0;

	if ((uintptr_t)dst & 4) {
		dst[0] = src[0];
		i = 1;
	}
	for (; i + 1 < n; i += 2)
		*(volatile u64_alias *)(dst + i) = (uint64_t)src[i] | (uint64_t)src[i + 1] << 32;
	if (i < n)
		dst[i] = src[i];
}

static void row_out(int y)
{
	const int s = ss.scale, n = ss.w * s;
	volatile uint32_t *dst = ss.origin + (long)y * s * PIX_PITCH;

	for (int k = 0; k < s; k++, dst += PIX_PITCH)
		fb_row(dst, line, n);
}

/* One source pixel: scale copies in the line buffer; a row that is complete goes to the picture. */
static inline void cur_put(struct cursor *c, uint32_t px)
{
	uint32_t *q = line + c->x * c->scale;

	for (int k = 0; k < c->scale; k++)
		q[k] = px;
	if (++c->x == c->w) {
		row_out(c->y);
		c->x = 0;
		c->y++;
	}
}

static void frame_begin(uint32_t number)
{
	ss.cur_frame = number;
	ss.c.x = 0;
	ss.c.y = 0;
	ss.c.w = ss.w;
	ss.c.scale = ss.scale;
	ss.has_pend = 0;
}

/* SWVS: picture geometry, screen cleared to black. w and h are validated by hdr_valid. */
static void stream_start(uint32_t w, uint32_t h, uint32_t fmt)
{
	int s = FB_W / (int)w;
	int sh = FB_H / (int)h;
	int x0, y0;

	if (sh < s)
		s = sh;
	x0 = (FB_W - (int)w * s) / 2;
	y0 = (FB_H - (int)h * s) / 2;
	ss.origin = con_fb + y0 * PIX_PITCH + x0;
	ss.fmt = (int)fmt;
	ss.bpp = fmt == FMT_PAL8 ? 1 : 2;
	ss.w = (int)w;
	ss.h = (int)h;
	ss.scale = s;
	for (int i = 0; i < 256; i++)
		ss.pal[i] = 0xff000000U;
	ss.active = 1;
	ss.ended = 0;
	ss.end_action = 0;
	con_clear(0xff000000U);
}

/* A complete 16-byte header that the current state accepts. */
static int hdr_valid(void)
{
	const uint8_t *h = ss.hdr;
	uint32_t a = rd32le(h + 4), b = rd32le(h + 8), len = rd32le(h + 12);

	switch (magic_of(h)) {
	case MSG_START:
		return len == 0 && (a >> 16) >= 1 && (a >> 16) <= (uint32_t)FB_W &&
		       (a & 0xffff) >= 1 && (a & 0xffff) <= (uint32_t)FB_H &&
		       (b == FMT_RGB565 || b == FMT_PAL8);
	case MSG_PAL:
		return ss.active && ss.fmt == FMT_PAL8 && len == PAL_BYTES;
	case MSG_FRAME:
		return ss.active && len == (uint32_t)(ss.w * ss.h * ss.bpp);
	case MSG_END:
		return ss.active && len == 0 && a <= 1;
	default:
		return 0;
	}
}

static void hdr_dispatch(void)
{
	const uint8_t *h = ss.hdr;
	uint32_t a = rd32le(h + 4), b = rd32le(h + 8), len = rd32le(h + 12);

	switch (magic_of(h)) {
	case MSG_START:
		stream_start(a >> 16, a & 0xffff, b);
		break;
	case MSG_PAL:
		ss.state = ST_PAL;
		ss.left = PAL_BYTES;
		ss.acc = 0;
		break;
	case MSG_FRAME:
		frame_begin(a);
		ss.state = ST_FRAME;
		ss.left = len;
		break;
	case MSG_END:
		ss.ended = 1;
		ss.end_action = (int)a;
		break;
	default:
		break;
	}
	ss.resync = 0;
	ss.hdr_fill = 0;
}

/* A bad byte at the head of the header buffer: count it once per run, slide the window by one. */
static void hdr_drop(void)
{
	if (!ss.resync) {
		ss.resync = 1;
		ss.proto_err++;
	}
	for (uint32_t i = 1; i < ss.hdr_fill; i++)
		ss.hdr[i - 1] = ss.hdr[i];
	ss.hdr_fill--;
}

/* One header byte. Until a known magic is at the head, bytes are dropped; a known magic with a bad header is dropped too. */
static void hdr_add(uint8_t c)
{
	ss.hdr[ss.hdr_fill++] = c;
	for (;;) {
		if (ss.hdr_fill < 4)
			return;
		if (magic_of(ss.hdr) == MSG_NONE) {
			hdr_drop();
			continue;
		}
		if (ss.hdr_fill < HDR_SIZE)
			return;
		if (!hdr_valid()) {
			hdr_drop();
			continue;
		}
		hdr_dispatch();
		return;
	}
}

/* Palette bytes: each entry is 4 bytes little-endian 0x00RRGGBB, stored with alpha 0xff. */
static void pal_feed(const uint8_t *d, uint32_t n)
{
	uint32_t pos = PAL_BYTES - ss.left;

	for (uint32_t i = 0; i < n; i++, pos++) {
		ss.acc |= (uint32_t)d[i] << (8 * (pos & 3));
		if ((pos & 3) == 3) {
			ss.pal[pos >> 2] = ss.acc | 0xff000000U;
			ss.acc = 0;
		}
	}
}

/*
 * Frame bytes into the line buffer, rows to the picture as they complete: 1 byte
 * per pixel (PAL8) or 2 (RGB565, a split pixel waits for its second byte).
 */
static void frame_feed(const uint8_t *d, uint32_t n)
{
	struct cursor c = ss.c;
	uint32_t i = 0;

	if (ss.fmt == FMT_PAL8) {
		for (; i < n; i++)
			cur_put(&c, ss.pal[d[i]]);
	} else {
		if (ss.has_pend && n) {
			cur_put(&c, rgb565_to_argb(ss.pend | (uint32_t)d[0] << 8));
			ss.has_pend = 0;
			i = 1;
		}
		for (; i + 1 < n; i += 2)
			cur_put(&c, rgb565_to_argb((uint32_t)d[i] | (uint32_t)d[i + 1] << 8));
		if (i < n) {
			ss.pend = d[i];
			ss.has_pend = 1;
		}
	}
	ss.c = c;
}

/* The payload of the current message is complete. */
static void payload_done(void)
{
	if (ss.state == ST_FRAME) {
		ss.frames++;
		ss.last_frame = ss.cur_frame;
	}
	ss.state = ST_HDR;
}

/* Bytes from one USB transfer (usbdev_poll callback); any split point is valid. */
static void stream_feed(const uint8_t *d, uint32_t n)
{
	ss.bytes += n;
	while (n) {
		if (ss.state == ST_HDR) {
			hdr_add(*d++);
			n--;
			continue;
		}
		uint32_t k = n < ss.left ? n : ss.left;

		if (ss.state == ST_PAL)
			pal_feed(d, k);
		else
			frame_feed(d, k);
		d += k;
		n -= k;
		ss.left -= k;
		if (ss.left == 0)
			payload_done();
	}
}

/* usbdev.c callback: one USB transfer. Its time counts as busy (status line busy=). */
static void stream_rx(const uint8_t *data, uint32_t len)
{
	uint64_t t0 = ust_now();

	stream_feed(data, len);
	ss.busy += ust_now() - t0;
}

static void stream_reset(void)
{
	ss.hdr_fill = 0;
	ss.state = ST_HDR;
	ss.left = 0;
	ss.acc = 0;
	ss.resync = 0;
	ss.active = 0;
	ss.ended = 0;
	ss.end_action = 0;
	ss.has_pend = 0;
	ss.frames = 0;
	ss.last_frame = 0;
	ss.proto_err = 0;
	ss.bytes = 0;
	ss.busy = 0;
}

/* Text helpers: NUL-terminated, return the new end. */
static char *put_str(char *p, const char *s)
{
	while (*s)
		*p++ = *s++;
	*p = 0;
	return p;
}

static char *put_dec(char *p, uint64_t v)
{
	char t[21];
	int n = 0;

	do {
		t[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	while (n)
		*p++ = t[--n];
	*p = 0;
	return p;
}

/* v in tenths: "12.3" */
static char *put_tenths(char *p, uint64_t v)
{
	p = put_dec(p, v / 10);
	*p++ = '.';
	*p++ = (char)('0' + v % 10);
	*p = 0;
	return p;
}

static char *put_hex(char *p, uint32_t v, int digits)
{
	static const char hex[] = "0123456789abcdef";

	for (int i = 0; i < digits; i++)
		*p++ = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	*p = 0;
	return p;
}

/*
 * The status line (row 0): frames, fps, busy (share of the last second spent in
 * the USB callback) and MB/s over that second, protocol errors, USB state bits.
 */
static void status_draw(uint64_t fps10, uint64_t busy_pct, uint64_t mbps10)
{
	char buf[96], *p = buf;

	p = put_str(p, "f=");
	p = put_dec(p, ss.frames);
	p = put_str(p, " fps=");
	p = put_tenths(p, fps10);
	p = put_str(p, " busy=");
	p = put_dec(p, busy_pct);
	p = put_str(p, "% MB/s=");
	p = put_tenths(p, mbps10);
	p = put_str(p, " err=");
	p = put_dec(p, ss.proto_err);
	p = put_str(p, " usb=");
	put_hex(p, usbdev_state_bits(), 2);
	con_set_row(0);
	con_clear_row(0);
	con_str("STREAM", buf);
}

/* A message on row 1, below the status line. */
static void note(const char *label, const char *s)
{
	con_set_row(1);
	con_clear_row(1);
	con_str(label, s);
}

static void note_rc(const char *label, int rc)
{
	con_set_row(1);
	con_clear_row(1);
	con_hex2(label, "rc=", (uint32_t)rc, 0, 0);
}

int usbstream_run(int wait_ms)
{
	struct mmu_info mi;
	uintptr_t lo, hi;
	const uint64_t f = ust_freq();
	const uint64_t wait_ticks = wait_ms > 0 ? (uint64_t)wait_ms * f / 1000 : 0;
	uint64_t t_init, t, t_next, t_prev, t_unconf = 0;
	uint32_t frames_prev = 0;
	uint64_t bytes_prev = 0, busy_prev = 0;
	int rc, conf_seen = 0, holding = 0;

	stream_reset();
	usbdev_dma_window(&lo, &hi);
	mmu_set_nc_window(lo, hi);
	rc = mmu_enable(&mi);
	if (rc) {
		note_rc("MMU", rc);
		return 0;
	}
	rc = usbdev_init(stream_rx);
	if (rc) {
		usbdev_stop();		/* back to the ABL state */
		note_rc("USB", rc);
		mmu_disable(&mi);
		return 0;
	}

	t_init = ust_now();
	t_prev = t_init;
	t_next = t_init + f;
	for (;;) {
		usbdev_poll();
		t = ust_now();
		if (ss.ended) {
			if (ss.end_action == 1) {
				note("STREAM", "done: booting XNU");
				usbdev_stop();
				mmu_disable(&mi);
				return 1;
			}
			if (!holding) {
				holding = 1;
				note("STREAM", "done: holding");
			}
		} else if (!ss.active) {
			if (t - t_init >= wait_ticks) {
				usbdev_stop();
				mmu_disable(&mi);
				return 0;
			}
		} else {
			holding = 0;
			if (usbdev_configured()) {
				conf_seen = 1;
				t_unconf = 0;
			} else if (conf_seen) {
				if (t_unconf == 0)
					t_unconf = t;
				else if (t - t_unconf > DISCONNECT_S * f) {
					note("STREAM", "done: host gone, booting XNU");
					usbdev_stop();
					mmu_disable(&mi);
					return 1;
				}
			}
		}
		if (t >= t_next) {
			t_next = t + f;
			if (ss.active && t > t_prev) {
				uint64_t dt = t - t_prev;
				uint64_t fps10 = (uint64_t)(ss.frames - frames_prev) * 10 * f / dt;
				uint64_t mbps10 = (ss.bytes - bytes_prev) * 10 * f / dt / (1024 * 1024);
				uint64_t busy_pct = (ss.busy - busy_prev) * 100 / dt;

				if (busy_pct > 100)
					busy_pct = 100;
				status_draw(fps10, busy_pct, mbps10);
				t_prev = t;
				frames_prev = ss.frames;
				bytes_prev = ss.bytes;
				busy_prev = ss.busy;
			}
		}
	}
}
