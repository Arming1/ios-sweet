/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_set_row(int r)
{
	row = r;
}

void con_clear_row(int r)
{
	int y0 = MARGIN + r * CH_H;
	for (int y = y0; y < y0 + CH_H && y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = con_bg;
	}
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}

void con_logo(const uint32_t *pal, const uint8_t *rle, uint32_t rle_len, int w, int h, int scale, int y0)
{
	int x0 = (FB_W - w * scale) / 2;
	int x = 0, y = 0;
	for (uint32_t k = 0; k + 1 < rle_len && y < h; k += 2) {
		uint32_t c = pal[rle[k + 1]];
		for (int n = rle[k]; n > 0 && y < h; n--) {
			int px = x0 + x * scale, py = y0 + y * scale;
			if (px >= 0 && px + scale <= FB_W && py >= 0 && py + scale <= FB_H)
				for (int sy = 0; sy < scale; sy++) {
					volatile uint32_t *p = con_fb + (py + sy) * (FB_STRIDE / 4) + px;
					for (int sx = 0; sx < scale; sx++)
						p[sx] = c;
				}
			if (++x == w) {
				x = 0;
				y++;
			}
		}
	}
}

/* one scale x scale block of the framebuffer, top-left at p */
static inline void video_block(volatile uint32_t *p, int scale, uint32_t c)
{
	for (int sy = 0; sy < scale; sy++, p += FB_STRIDE / 4)
		for (int sx = 0; sx < scale; sx++)
			p[sx] = c;
}

/*
 * Ops as written by tools/mkvideo.py: SKIP (op < 0x80) moves past unchanged
 * pixels, RUN (0x80..0xbf) writes one index n times, LITERAL (>= 0xc0) writes
 * n indices. Source pixels are counted in raster order and wrap at w, like
 * the stream they encode.
 */
void con_video_frame(const uint32_t *pal, const uint8_t *ops, const uint8_t *end,
		     int w, int h, int scale, int x0, int y0)
{
	const int row_step = scale * (FB_STRIDE / 4);	/* words per source row */
	volatile uint32_t *row = con_fb + y0 * (FB_STRIDE / 4) + x0;	/* block at (0, y) */
	volatile uint32_t *p = row;				/* block at (x, y) */
	int x = 0, y = 0;

	while (ops < end && y < h) {
		unsigned op = *ops++;
		if (op < 0x80) {
			x += op + 1;
			while (x >= w) {
				x -= w;
				y++;
				row += row_step;
			}
			p = row + x * scale;
			continue;
		}
		uint32_t c = 0;
		int n = (op & 0x3f) + 1;
		if (op < 0xc0)
			c = pal[*ops++];
		for (; n > 0; n--) {
			if (op >= 0xc0)
				c = pal[*ops++];
			video_block(p, scale, c);
			if (++x == w) {
				x = 0;
				y++;
				row += row_step;
				p = row;
			} else {
				p += scale;
			}
		}
	}
}
