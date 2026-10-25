#!/usr/bin/env python3
"""
Windows host sender for the shim's USB experiment U2 (shim EXPERIMENT=16).

The phone enumerates as a vendor-class USB 2.0 high-speed device. Windows binds
WinUSB by itself, from the MS OS 2.0 descriptors, and the device interface GUID
below is the handle this script opens. Bulk OUT pipe 0x01 receives chunks that
start with b"SWV0".

  python -I tools/win-usbstream.py --list            enumerate only (dry run)
  python -I tools/win-usbstream.py --wait            wait for the device, print its path
  python -I tools/win-usbstream.py --bench 30        send for 30 s, MB/s every second
  python -I tools/win-usbstream.py --bench 30 --chunk 65536

Live stream (shim SHIM_USBSTREAM, usbstream.c): the same device, messages with
the 16-byte header "magic, a, b, len" (little-endian):
  SWVS start (a = w << 16 | h, b = 1 RGB565 / 2 PAL8), SWVP palette (PAL8),
  SWVF frame (a = frame number, len = w*h*bpp), SWVE end (a = 0 hold, 1 boot).

  python -I tools/win-usbstream.py --play clip.mp4                 60 fps, 360x640, RGB565, hold
  python -I tools/win-usbstream.py --play clip.mp4 --fps 30 --size 720x1280 --format pal8 --end boot
  python -I tools/win-usbstream.py --play clip.mp4 --loop 3       plays the clip 3 times in one stream
  python -I tools/win-usbstream.py --testpattern 10                generated bars and frame counter, no ffmpeg

ffmpeg: the FFMPEG environment variable (path to ffmpeg.exe), else PATH.
Python 3 standard library only (ctypes over SetupAPI and WinUSB).
"""
import argparse
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import time
import uuid
from ctypes import wintypes

DEVICE_GUID = "{3B6C5B1E-5A0A-4C2E-9F4B-5357454554F1}"
PIPE_OUT = 0x01
MAGIC = b"SWV0"
DEFAULT_CHUNK = 32768

# live stream protocol (usbstream.c)
HDR = struct.Struct("<4sIII")
FMT_RGB565 = 1
FMT_PAL8 = 2
PAL_BYTES = 1024
MAX_W = 1440
MAX_H = 3200

DIGCF_PRESENT = 0x00000002
DIGCF_DEVICEINTERFACE = 0x00000010
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
ERROR_INSUFFICIENT_BUFFER = 122
ERROR_IO_PENDING = 997
PIPE_TRANSFER_TIMEOUT = 0x03
PIPE_TIMEOUT_MS = 5000
INVALID_HANDLE = ctypes.c_void_p(-1).value


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVINFO_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("ClassGuid", GUID),
                ("DevInst", wintypes.DWORD), ("Reserved", ctypes.c_size_t)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wintypes.DWORD), ("InterfaceClassGuid", GUID),
                ("Flags", wintypes.DWORD), ("Reserved", ctypes.c_size_t)]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_size_t), ("InternalHigh", ctypes.c_size_t),
                ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
                ("hEvent", ctypes.c_void_p)]


def guid_from_text(text):
    u = uuid.UUID(text)
    g = GUID()
    g.Data1 = u.time_low
    g.Data2 = u.time_mid
    g.Data3 = u.time_hi_version
    for i, b in enumerate(u.bytes[8:16]):
        g.Data4[i] = b
    return g


class Api:
    """The DLL entry points used here, with their prototypes (lazy: --help works anywhere)."""

    def __init__(self):
        self.setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
        self.winusb = ctypes.WinDLL("winusb", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        p = ctypes.c_void_p
        s = self.setupapi
        s.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, p, wintypes.DWORD]
        s.SetupDiGetClassDevsW.restype = p
        s.SetupDiEnumDeviceInterfaces.argtypes = [p, p, ctypes.POINTER(GUID), wintypes.DWORD,
                                                  ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)]
        s.SetupDiEnumDeviceInterfaces.restype = wintypes.BOOL
        s.SetupDiGetDeviceInterfaceDetailW.argtypes = [p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA), p,
                                                       wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), p]
        s.SetupDiGetDeviceInterfaceDetailW.restype = wintypes.BOOL
        s.SetupDiDestroyDeviceInfoList.argtypes = [p]
        s.SetupDiDestroyDeviceInfoList.restype = wintypes.BOOL

        k = self.kernel32
        k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, p,
                                  wintypes.DWORD, wintypes.DWORD, p]
        k.CreateFileW.restype = p
        k.CreateEventW.argtypes = [p, wintypes.BOOL, wintypes.BOOL, wintypes.LPCWSTR]
        k.CreateEventW.restype = p
        k.CloseHandle.argtypes = [p]
        k.CloseHandle.restype = wintypes.BOOL

        w = self.winusb
        w.WinUsb_Initialize.argtypes = [p, p]          # (file handle, address of the interface handle)
        w.WinUsb_Initialize.restype = wintypes.BOOL
        w.WinUsb_Free.argtypes = [p]
        w.WinUsb_Free.restype = wintypes.BOOL
        w.WinUsb_SetPipePolicy.argtypes = [p, ctypes.c_ubyte, wintypes.ULONG, wintypes.ULONG, p]
        w.WinUsb_SetPipePolicy.restype = wintypes.BOOL
        w.WinUsb_WritePipe.argtypes = [p, ctypes.c_ubyte, p, wintypes.ULONG, p, p]
        w.WinUsb_WritePipe.restype = wintypes.BOOL
        w.WinUsb_GetOverlappedResult.argtypes = [p, p, p, wintypes.BOOL]
        w.WinUsb_GetOverlappedResult.restype = wintypes.BOOL


def device_paths(api):
    """Device interface paths of every present device with DEVICE_GUID."""
    guid = guid_from_text(DEVICE_GUID)
    info = api.setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None,
                                             DIGCF_PRESENT | DIGCF_DEVICEINTERFACE)
    if info == INVALID_HANDLE or info is None:
        raise OSError("SetupDiGetClassDevsW failed: %d" % ctypes.get_last_error())
    paths = []
    try:
        index = 0
        while True:
            did = SP_DEVICE_INTERFACE_DATA()
            did.cbSize = ctypes.sizeof(did)
            if not api.setupapi.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(guid), index,
                                                            ctypes.byref(did)):
                if ctypes.get_last_error() == 259:	# ERROR_NO_MORE_ITEMS
                    break
                raise OSError("SetupDiEnumDeviceInterfaces failed: %d" % ctypes.get_last_error())
            index += 1
            required = wintypes.DWORD(0)
            api.setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(did), None, 0,
                                                          ctypes.byref(required), None)
            if ctypes.get_last_error() != ERROR_INSUFFICIENT_BUFFER:
                raise OSError("detail size query failed: %d" % ctypes.get_last_error())
            buf = ctypes.create_string_buffer(required.value)
            # SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize: 8 on 64-bit Windows, 6 on 32-bit
            struct.pack_into("<I", buf, 0, 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6)
            if not api.setupapi.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(did),
                                                                 ctypes.cast(buf, ctypes.c_void_p),
                                                                 required.value, None, None):
                raise OSError("SetupDiGetDeviceInterfaceDetailW failed: %d" % ctypes.get_last_error())
            paths.append(ctypes.wstring_at(ctypes.addressof(buf) + 4))
    finally:
        api.setupapi.SetupDiDestroyDeviceInfoList(info)
    return paths


def wait_for_device(api, timeout):
    """Poll until a device appears; returns its path or None after timeout seconds."""
    deadline = time.monotonic() + timeout
    announced = False
    while True:
        paths = device_paths(api)
        if paths:
            return paths[0]
        if time.monotonic() >= deadline:
            return None
        if not announced:
            print("waiting for the device (%s) ..." % DEVICE_GUID, flush=True)
            announced = True
        time.sleep(0.5)


class Device:
    def __init__(self, api, path):
        self.api = api
        self.file = api.kernel32.CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                             OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)
        if self.file == INVALID_HANDLE or self.file is None:
            raise OSError("CreateFileW failed: %d" % ctypes.get_last_error())
        self.usb = ctypes.c_void_p()
        if not api.winusb.WinUsb_Initialize(self.file, ctypes.addressof(self.usb)):
            err = ctypes.get_last_error()
            api.kernel32.CloseHandle(self.file)
            raise OSError("WinUsb_Initialize failed: %d (is WinUSB bound?)" % err)
        timeout = wintypes.ULONG(PIPE_TIMEOUT_MS)
        api.winusb.WinUsb_SetPipePolicy(self.usb, PIPE_OUT, PIPE_TRANSFER_TIMEOUT, ctypes.sizeof(timeout),
                                        ctypes.addressof(timeout))
        self.event = api.kernel32.CreateEventW(None, False, False, None)
        self.ov = OVERLAPPED()
        self.ov.hEvent = self.event
        self.transferred = wintypes.ULONG(0)

    def write(self, buf):
        """Synchronous bulk OUT write of the whole buffer; returns bytes transferred."""
        w = self.api.winusb
        ok = w.WinUsb_WritePipe(self.usb, PIPE_OUT, ctypes.addressof(buf), len(buf),
                                ctypes.addressof(self.transferred), ctypes.addressof(self.ov))
        if not ok:
            err = ctypes.get_last_error()
            if err != ERROR_IO_PENDING:
                raise OSError("WinUsb_WritePipe failed: %d" % err)
            if not w.WinUsb_GetOverlappedResult(self.usb, ctypes.addressof(self.ov),
                                                ctypes.addressof(self.transferred), True):
                raise OSError("WinUsb_GetOverlappedResult failed: %d" % ctypes.get_last_error())
        return self.transferred.value

    def close(self):
        self.api.winusb.WinUsb_Free(self.usb)
        if self.event:
            self.api.kernel32.CloseHandle(self.event)
        self.api.kernel32.CloseHandle(self.file)


def cmd_list(api):
    paths = device_paths(api)
    print("%d device(s) with %s" % (len(paths), DEVICE_GUID))
    for p in paths:
        print("  " + p)
    return 0


def cmd_wait(api, timeout):
    path = wait_for_device(api, timeout)
    if path is None:
        print("no device after %d s" % timeout)
        return 1
    print(path)
    return 0


def cmd_bench(api, seconds, chunk, timeout):
    path = wait_for_device(api, timeout)
    if path is None:
        print("no device after %d s" % timeout)
        return 1
    dev = Device(api, path)
    try:
        buf = (ctypes.c_char * chunk)()
        pattern = bytes(i & 0xff for i in range(chunk))
        buf[0:chunk] = pattern
        buf[0:4] = MAGIC
        t0 = time.monotonic()
        report_at = t0 + 1.0
        report_bytes = 0
        total = 0
        seq = 0
        while True:
            now = time.monotonic()
            if now - t0 >= seconds:
                break
            buf[0:4] = MAGIC
            buf[4:8] = seq.to_bytes(4, "little")
            try:
                n = dev.write(buf)
            except OSError as e:
                print("t=%.1f s: %s" % (now - t0, e))
                return 1
            total += n
            seq += 1
            now = time.monotonic()
            if now >= report_at:
                rate = (total - report_bytes) / (now - (report_at - 1.0)) / (1 << 20)
                print("t=%.1f s  %.2f MB/s  total %.1f MB" % (now - t0, rate, total / (1 << 20)), flush=True)
                report_bytes = total
                report_at += 1.0
        elapsed = time.monotonic() - t0
        print("done: %d chunks, %.1f MB in %.2f s = %.2f MB/s"
              % (seq, total / (1 << 20), elapsed, total / elapsed / (1 << 20)))
        return 0
    finally:
        dev.close()


# ---- live stream: --play (ffmpeg) and --testpattern (generated) ----

END_ACTION = {"hold": 0, "boot": 1}

# 5x7 glyphs of the frame counter ('#' = ink)
DIGITS = {
    "0": [" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "],
    "1": ["  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "],
    "2": [" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"],
    "3": ["#####", "   # ", "  #  ", "   # ", "    #", "#   #", " ### "],
    "4": ["   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "],
    "5": ["#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "],
    "6": ["  ## ", " #   ", "#    ", "#### ", "#   #", "#   #", " ### "],
    "7": ["#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "],
    "8": [" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "],
    "9": [" ### ", "#   #", "#   #", " ####", "    #", "   # ", " ##  "],
}
PATTERN_BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
                (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def parse_size(text):
    try:
        w_text, h_text = text.lower().split("x")
        w, h = int(w_text), int(h_text)
    except ValueError:
        raise argparse.ArgumentTypeError("--size must be WxH, e.g. 360x640")
    if not (1 <= w <= MAX_W and 1 <= h <= MAX_H):
        raise argparse.ArgumentTypeError("--size must be at most %dx%d" % (MAX_W, MAX_H))
    return w, h


def find_ffmpeg():
    env = os.environ.get("FFMPEG")
    if env:
        if not os.path.isfile(env):
            raise OSError("FFMPEG=%s is not a file" % env)
        return env
    exe = shutil.which("ffmpeg")
    if exe is None:
        raise OSError("ffmpeg not found: set FFMPEG to the path of ffmpeg.exe, or put it on PATH")
    return exe


def send(dev, magic, a=0, b=0, payload=b""):
    """One WinUsb_WritePipe call for a whole message: 16-byte header, then payload."""
    buf = bytearray(HDR.pack(magic, a, b, len(payload)))
    buf += payload
    cbuf = (ctypes.c_char * len(buf)).from_buffer(buf)
    n = dev.write(cbuf)
    if n != len(buf):
        raise OSError("short write: %d of %d bytes" % (n, len(buf)))
    return n


def wait_until(when):
    """Sleeps until perf_counter() reaches when; the last 2 ms are spun for accuracy."""
    while True:
        rem = when - time.perf_counter()
        if rem <= 0:
            return
        if rem > 0.002:
            time.sleep(rem - 0.001)


def read_exact(stream, n):
    """n bytes from a pipe, or None at end of stream (a partial frame is dropped)."""
    parts = []
    got = 0
    while got < n:
        chunk = stream.read(n - got)
        if not chunk:
            return None
        parts.append(chunk)
        got += len(chunk)
    return b"".join(parts)


def palette_payload(raw):
    """ffmpeg palette entries are little-endian 0xAARRGGBB; the shim takes 0x00RRGGBB (its alpha is forced)."""
    pal = bytearray(raw)
    pal[3::4] = bytes(len(pal) // 4)
    return bytes(pal)


def ffmpeg_frames(ffmpeg, path, fps, w, h, fmt, loops):
    """Yields (frame bytes, palette payload or None) for every frame ffmpeg writes."""
    vf = "fps=%g,scale=%d:%d:flags=lanczos" % (fps, w, h)
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error",
           "-stream_loop", str(loops - 1), "-i", path, "-vf", vf, "-f", "rawvideo"]
    if fmt == "rgb565":
        cmd += ["-pix_fmt", "rgb565le"]
        frame_len = w * h * 2
    else:
        # pal8: w*h index bytes, then the 1024-byte palette, once per frame
        cmd += ["-pix_fmt", "pal8"]
        frame_len = w * h + PAL_BYTES
    cmd.append("-")
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    clean = False
    try:
        while True:
            data = read_exact(proc.stdout, frame_len)
            if data is None:
                break
            if fmt == "rgb565":
                yield data, None
            else:
                yield data[:w * h], palette_payload(data[w * h:])
        clean = True
    finally:
        if proc.poll() is None:
            proc.kill()
        proc.stdout.close()
        err = proc.stderr.read().decode("utf-8", "replace").strip()
        rc = proc.wait()
        if clean and rc != 0:
            raise OSError("ffmpeg failed (exit %d): %s" % (rc, err))


def rgb565_bytes(r, g, b):
    return struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))


def draw_counter(frame, w, number, scale, bpp, ink, paper):
    """Six decimal digits at the top left, 5x7 glyphs scaled by scale."""
    text = "%06d" % (number % 1000000)
    for d, ch in enumerate(text):
        glyph = DIGITS[ch]
        for gy in range(7):
            run = b"".join((ink if gx < 5 and glyph[gy][gx] == "#" else paper) * scale for gx in range(6))
            for sy in range(scale):
                y = 2 * scale + gy * scale + sy
                start = (y * w + 2 * scale + d * 6 * scale) * bpp
                frame[start:start + len(run)] = run


def pattern_frames(w, h, fmt, count):
    """Yields (frame, palette payload or None): moving colour bars and a frame counter."""
    if fmt == "rgb565":
        bpp = 2
        bars = [rgb565_bytes(*c) for c in PATTERN_BARS]
        ink, paper = rgb565_bytes(255, 255, 255), rgb565_bytes(0, 0, 0)
        palette = None
    else:
        bpp = 1
        bars = [bytes([i]) for i in range(8)]		# palette 0..7 = the bars
        ink, paper = bytes([9]), bytes([8])			# palette 8 black, 9 white
        entries = list(PATTERN_BARS) + [(0, 0, 0), (255, 255, 255)]
        entries += [(0, 0, 0)] * (256 - len(entries))
        palette = b"".join(struct.pack("<I", (r << 16) | (g << 8) | b) for r, g, b in entries)
    bar_w = max(1, w // 8)
    period = 8 * bar_w
    unit = b"".join(bar * bar_w for bar in bars)
    rep = unit * (w // period + 3)
    scale = max(1, w // 60)
    for i in range(count):
        off = (i * 4) % period			# bars move 4 pixels per frame
        row = rep[off * bpp:(off + w) * bpp]
        frame = bytearray(row * h)
        draw_counter(frame, w, i, scale, bpp, ink, paper)
        yield bytes(frame), palette


def stream(dev, frames, w, h, fmt, fps, end_action):
    """SWVS, SWVP when the palette changes, one SWVF per frame at its slot t0 + i/fps, then SWVE.

    A frame that starts more than one frame period after its slot counts as late
    (it is sent at once, and the later slots are not moved).
    """
    fmt_code = FMT_RGB565 if fmt == "rgb565" else FMT_PAL8
    send(dev, b"SWVS", (w << 16) | h, fmt_code)
    period = 1.0 / fps
    t0 = time.perf_counter()
    last_pal = None
    total_frames = total_bytes = total_late = 0
    sec_start = t0
    sec_frames = sec_bytes = sec_late = 0
    for i, (frame, pal) in enumerate(frames):
        if pal is not None and pal != last_pal:
            send(dev, b"SWVP", 0, 0, pal)
            last_pal = pal
        due = t0 + i * period
        wait_until(due)
        if time.perf_counter() - due > period:
            sec_late += 1
            total_late += 1
        n = send(dev, b"SWVF", i, 0, frame)
        total_frames += 1
        total_bytes += n
        sec_frames += 1
        sec_bytes += n
        now = time.perf_counter()
        if now - sec_start >= 1.0:
            span = now - sec_start
            print("t=%6.1f s  frames=%6d  fps=%6.2f  MB/s=%6.2f  late=%d"
                  % (now - t0, total_frames, sec_frames / span, sec_bytes / span / (1 << 20), sec_late),
                  flush=True)
            sec_start = now
            sec_frames = sec_bytes = sec_late = 0
    send(dev, b"SWVE", end_action, 0)
    elapsed = time.perf_counter() - t0
    print("done: %d frames, %.1f MB in %.2f s = %.2f MB/s, late %d; SWVE a=%d (%s)"
          % (total_frames, total_bytes / (1 << 20), elapsed,
             total_bytes / max(elapsed, 1e-9) / (1 << 20), total_late, end_action,
             "hold" if end_action == 0 else "boot"))
    if total_frames == 0:
        print("no frames were read")
        return 1
    return 0


def cmd_play(api, args):
    if not os.path.isfile(args.play):
        print("no such file: %s" % args.play, file=sys.stderr)
        return 2
    ffmpeg = find_ffmpeg()
    path = wait_for_device(api, args.timeout)
    if path is None:
        print("no device after %d s" % args.timeout)
        return 1
    w, h = args.size
    dev = Device(api, path)
    frames = ffmpeg_frames(ffmpeg, args.play, args.fps, w, h, args.format, args.loop)
    try:
        return stream(dev, frames, w, h, args.format, args.fps, END_ACTION[args.end])
    finally:
        frames.close()
        dev.close()


def cmd_testpattern(api, args):
    w, h = args.size
    count = max(1, int(round(args.testpattern * args.fps)))
    path = wait_for_device(api, args.timeout)
    if path is None:
        print("no device after %d s" % args.timeout)
        return 1
    dev = Device(api, path)
    frames = pattern_frames(w, h, args.format, count)
    try:
        return stream(dev, frames, w, h, args.format, args.fps, END_ACTION[args.end])
    finally:
        dev.close()


def main(argv):
    ap = argparse.ArgumentParser(description="Send to the shim's WinUSB device (U2 bench, S1 live stream).")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--list", action="store_true", help="enumerate devices and exit (no transfer)")
    mode.add_argument("--wait", action="store_true", help="wait for the device, print its path")
    mode.add_argument("--bench", type=float, metavar="SECONDS", help="send for SECONDS, MB/s every second")
    mode.add_argument("--play", metavar="FILE", help="stream a video file (ffmpeg) as frames, live")
    mode.add_argument("--testpattern", type=float, metavar="SECONDS",
                      help="stream generated colour bars and a frame counter, no ffmpeg")
    ap.add_argument("--chunk", type=int, default=DEFAULT_CHUNK, help="bytes per bulk OUT write (default %d)"
                    % DEFAULT_CHUNK)
    ap.add_argument("--timeout", type=float, default=30.0, help="seconds to wait for the device (default 30)")
    ap.add_argument("--fps", type=float, default=60.0, help="--play/--testpattern frame rate (default 60)")
    ap.add_argument("--size", type=parse_size, default=(360, 640),
                    help="--play/--testpattern picture size WxH (default 360x640, at most 1440x3200)")
    ap.add_argument("--format", choices=("rgb565", "pal8"), default="rgb565",
                    help="--play/--testpattern pixel format (default rgb565)")
    ap.add_argument("--end", choices=("hold", "boot"), default="hold",
                    help="--play/--testpattern after the last frame: keep the picture or boot XNU (default hold)")
    ap.add_argument("--loop", type=int, default=1, help="--play: play the file this many times (default 1)")
    args = ap.parse_args(argv)
    if sys.platform != "win32":
        print("win-usbstream.py runs on Windows only (SetupAPI and WinUSB)", file=sys.stderr)
        return 2
    if args.chunk <= 0 or args.chunk > (1 << 20):
        ap.error("--chunk must be 1..1048576")
    if args.bench is not None and args.bench <= 0:
        ap.error("--bench SECONDS must be positive")
    if args.testpattern is not None and args.testpattern <= 0:
        ap.error("--testpattern SECONDS must be positive")
    if args.play is not None or args.testpattern is not None:
        if not 0 < args.fps <= 120:
            ap.error("--fps must be in (0, 120]")
        if args.loop < 1:
            ap.error("--loop must be at least 1")
    if args.testpattern is not None and args.loop != 1:
        ap.error("--loop applies to --play only")
    api = Api()
    if args.list:
        return cmd_list(api)
    if args.wait:
        return cmd_wait(api, args.timeout)
    if args.play is not None or args.testpattern is not None:
        try:
            if args.play is not None:
                return cmd_play(api, args)
            return cmd_testpattern(api, args)
        except OSError as e:
            print("error: %s" % e, file=sys.stderr)
            return 1
    return cmd_bench(api, args.bench, args.chunk, args.timeout)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
