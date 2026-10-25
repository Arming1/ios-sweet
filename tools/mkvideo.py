#!/usr/bin/env python3
"""Convert a video into a silent palette-indexed frame-op C header for the venus shim.

shim/build.sh runs it when SHIM_VIDEO_FILE is set. The source video and the
generated header are never committed to the repo (shim/out is ignored).

Usage: python -I tools/mkvideo.py <in-video> <out.h> [--w 360 --h 640 --fps 30 --max-seconds 20]

ffmpeg comes from the FFMPEG environment variable, else from PATH. The video is
scaled to WxH with one global 256-colour palette (palettegen/paletteuse), written
to a temporary directory as PNG frames and loaded back with Pillow (mode "P").

Each frame is encoded as ops over the raster-order index stream, compared with
the previous frame (frame 0 is compared with an all-unknown frame, so it has no
SKIP ops):
  0x00..0x7F  SKIP (op+1) pixels, unchanged
  0x80..0xBF  RUN (op&0x3f)+1 pixels of the single index byte that follows
  0xC0..0xFF  LITERAL (op&0x3f)+1 index bytes follow
shim/console.c con_video_frame() applies them. The encoder output is replayed
here and compared with every PNG before the header is written.
"""
import argparse
import glob
import operator
import os
import re
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

PAL_N = 256
OPS_PER_LINE = 16
SKIP_MAX = 128
RUN_MAX = 64
LIT_MAX = 64
RUN_RE = re.compile(rb'(.)\1{2,}', re.DOTALL)
UNCHANGED_RE = re.compile(rb'\x00+')


def encode_frame(cur, prev):
    """Greedy encoder. prev is None for frame 0 (no SKIP ops)."""
    n = len(cur)
    if prev is None:
        skips = []
    else:
        diff = bytes(map(operator.xor, cur, prev))
        skips = [m.span() for m in UNCHANGED_RE.finditer(diff)]
    runs = [m.span() for m in RUN_RE.finditer(cur)]

    ops = bytearray()
    si = ri = 0
    i = 0
    while i < n:
        while si < len(skips) and skips[si][1] <= i:
            si += 1
        if si < len(skips) and skips[si][0] <= i:
            end = min(skips[si][1], i + SKIP_MAX)
            ops.append(end - i - 1)
            i = end
            continue
        while ri < len(runs) and runs[ri][1] <= i:
            ri += 1
        if ri < len(runs) and runs[ri][0] <= i:
            end = min(runs[ri][1], i + RUN_MAX)
            ops += bytes((0x80 | (end - i - 1), cur[i]))
            i = end
            continue
        # literal: stop at the next unchanged span or the next run
        end = min(i + LIT_MAX, n)
        if si < len(skips):
            end = min(end, skips[si][0])
        if ri < len(runs):
            end = min(end, runs[ri][0])
        ops.append(0xC0 | (end - i - 1))
        ops += cur[i:end]
        i = end
    return bytes(ops)


def decode_frame(ops, prev, n):
    """Replay one frame's ops over prev (None for frame 0). Returns the new index state."""
    state = bytearray(n) if prev is None else bytearray(prev)
    pos = 0
    p = 0
    while p < len(ops):
        op = ops[p]
        p += 1
        if op < 0x80:
            if prev is None:
                raise RuntimeError('frame 0 has a SKIP op at pixel %d' % pos)
            k = op + 1
        elif op < 0xC0:
            k = (op & 0x3F) + 1
            if p >= len(ops):
                raise RuntimeError('RUN op without its index byte at pixel %d' % pos)
            if pos + k > n:
                raise RuntimeError('RUN past the end of the frame at pixel %d' % pos)
            state[pos:pos + k] = bytes((ops[p],)) * k
            p += 1
        else:
            k = (op & 0x3F) + 1
            if p + k > len(ops) or pos + k > n:
                raise RuntimeError('LITERAL overruns at pixel %d' % pos)
            state[pos:pos + k] = ops[p:p + k]
            p += k
        if op < 0x80 and pos + k > n:
            raise RuntimeError('SKIP past the end of the frame at pixel %d' % pos)
        pos += k
    if pos != n:
        raise RuntimeError('ops cover %d pixels, frame has %d' % (pos, n))
    return state


def run_ffmpeg(ffmpeg, src, w, h, fps, max_seconds, out_dir):
    vf = ('fps=%d,scale=%d:%d:flags=lanczos,split[a][b];'
          '[a]palettegen=max_colors=256:stats_mode=full[p];'
          '[b][p]paletteuse=dither=none') % (fps, w, h)
    cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-i', src,
           '-vf', vf, '-t', str(max_seconds), os.path.join(out_dir, '%06d.png')]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--w', type=int, default=360, help='frame width, pixels')
    ap.add_argument('--h', type=int, default=640, help='frame height, pixels')
    ap.add_argument('--fps', type=int, default=30, help='frames per second')
    ap.add_argument('--max-seconds', type=float, default=20, help='clip length limit')
    a = ap.parse_args()
    if a.w < 1 or a.h < 1 or a.fps < 1 or a.max_seconds <= 0:
        ap.error('--w, --h, --fps and --max-seconds must be positive')

    ffmpeg = os.environ.get('FFMPEG') or shutil.which('ffmpeg')
    if not ffmpeg:
        ap.error('ffmpeg not found: set FFMPEG or put it on PATH')

    n = a.w * a.h
    pal = None
    prev = None
    frame_ops = []
    with tempfile.TemporaryDirectory() as tmp:
        run_ffmpeg(ffmpeg, a.src, a.w, a.h, a.fps, a.max_seconds, tmp)
        files = sorted(glob.glob(os.path.join(tmp, '*.png')))
        if not files:
            raise RuntimeError('ffmpeg wrote no frames')

        for f in files:
            img = Image.open(f)
            if img.mode != 'P':
                raise RuntimeError('%s: expected palette mode P, got %s' % (f, img.mode))
            if img.size != (a.w, a.h):
                raise RuntimeError('%s: size %s, expected %dx%d' % (f, img.size, a.w, a.h))
            raw = img.getpalette() or []
            raw = list(raw) + [0] * (768 - len(raw))
            if pal is None:
                pal = raw
            elif raw != pal:
                raise RuntimeError('%s: palette differs from frame 0' % f)
            cur = img.tobytes()
            if len(cur) != n:
                raise RuntimeError('%s: %d index bytes, expected %d' % (f, len(cur), n))

            ops = encode_frame(cur, prev)
            state = decode_frame(ops, prev, n)
            if state != cur:
                raise RuntimeError('decoder self-check failed at frame %d' % len(frame_ops))
            frame_ops.append(ops)
            prev = cur
            img.close()

    frames = len(frame_ops)
    all_ops = b''.join(frame_ops)
    offs = [0]
    for ops in frame_ops:
        offs.append(offs[-1] + len(ops))

    colours = []
    for i in range(PAL_N):
        if 3 * i + 2 < len(pal):
            r, g, b = pal[3 * i:3 * i + 3]
            colours.append(0xff000000 | r << 16 | g << 8 | b)
        else:
            colours.append(0xff000000)

    lines = [
        '/* generated by tools/mkvideo.py: %dx%d, %d fps, %d frames (not committed) */' % (a.w, a.h, a.fps, frames),
        '#ifndef SHIM_VIDEO_H',
        '#define SHIM_VIDEO_H',
        '',
        '#include <stdint.h>',
        '',
        '#define VIDEO_W %d' % a.w,
        '#define VIDEO_H %d' % a.h,
        '#define VIDEO_FPS %d' % a.fps,
        '#define VIDEO_FRAMES %d' % frames,
        'static const uint32_t video_pal[256] = {',
    ]
    for i in range(0, PAL_N, 8):
        lines.append('\t' + ','.join('0x%08x' % v for v in colours[i:i + 8]) + ',')
    lines.append('};')
    lines.append('static const uint8_t video_ops[] = {')
    for i in range(0, len(all_ops), OPS_PER_LINE):
        lines.append('\t' + ','.join('0x%02x' % b for b in all_ops[i:i + OPS_PER_LINE]) + ',')
    lines.append('};')
    lines.append('static const uint32_t video_frame_off[VIDEO_FRAMES + 1] = {')
    for i in range(0, len(offs), 8):
        lines.append('\t' + ','.join('%d' % v for v in offs[i:i + 8]) + ',')
    lines.append('};')
    lines += ['', '#endif', '']

    out_dir = os.path.dirname(os.path.abspath(a.out))
    os.makedirs(out_dir, exist_ok=True)
    with open(a.out, 'w', newline='\n') as f:
        f.write('\n'.join(lines))

    print('frames=%d' % frames)
    print('total_bytes=%d' % len(all_ops))
    print('avg_bytes_per_frame=%.1f' % (len(all_ops) / frames))


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as e:
        print('error: %s' % e, file=sys.stderr)
        sys.exit(1)
