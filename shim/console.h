#pragma once
#include <stdint.h>

#define FB_BASE		0xe2a00000UL	/* venus: measured scanout buffer (VIG0) */
#define FB_W		1440
#define FB_H		3200
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8, matches mainline simplefb */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define DARKGREY	0xff202428U	/* venus: normal-run background (red read as alarming); panics stay BLUE */

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
/* fixed rows for status lines that are redrawn in place (row 0 is the top) */
void con_set_row(int r);
void con_clear_row(int r);	/* background-fill one text row (CH_H pixels high) */
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
/* label, then "a=xxxxxxxx b=xxxxxxxx" style pairs of 32-bit values */
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
/* palette-indexed RLE image, (run,index) pairs, drawn centred horizontally at row y0; each pixel as a scale x scale block; clipped to the screen */
void con_logo(const uint32_t *pal, const uint8_t *rle, uint32_t rle_len, int w, int h, int scale, int y0);
/* one video frame from tools/mkvideo.py ops (ops..end), w x h source pixels, each a scale x scale block with its top-left at (x0, y0); the caller keeps the picture on screen */
void con_video_frame(const uint32_t *pal, const uint8_t *ops, const uint8_t *end, int w, int h, int scale, int x0, int y0);
