#!/bin/bash
# Build the shim and wrap it in a boot.img v2 that mirrors the stock PixelOS header.
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra ${EXTRA_CFLAGS:-}"
mkdir -p out
# Optional boot logo (VENUS-PORT.md): SHIM_LOGO_IMAGE=/path/to/logo.png generates
# out/logo/logo.h (never committed) and defines SHIM_LOGO for shim.c.
if [ -n "${SHIM_LOGO_IMAGE:-}" ]; then
	[ -f "$SHIM_LOGO_IMAGE" ] || { echo "error: SHIM_LOGO_IMAGE not found: $SHIM_LOGO_IMAGE" >&2; exit 1; }
	mkdir -p out/logo
	python3 ../tools/mklogo.py "$SHIM_LOGO_IMAGE" out/logo/logo.h ${SHIM_LOGO_MAX:+--max "$SHIM_LOGO_MAX"}
	CFLAGS="$CFLAGS -DSHIM_LOGO -Iout/logo"
fi
# Optional silent video (SHIM_VIDEO_FILE=/path/to/clip): tools/mkvideo.py writes
# out/video/video.h (never committed) and defines SHIM_VIDEO for shim.c.
# SHIM_VIDEO_HOLD=1 selects the hold message (both builds hold for now).
if [ -n "${SHIM_VIDEO_FILE:-}" ]; then
	[ -f "$SHIM_VIDEO_FILE" ] || { echo "error: SHIM_VIDEO_FILE not found: $SHIM_VIDEO_FILE" >&2; exit 1; }
	mkdir -p out/video
	python3 ../tools/mkvideo.py "$SHIM_VIDEO_FILE" out/video/video.h
	CFLAGS="$CFLAGS -DSHIM_VIDEO -Iout/video"
	if [ "${SHIM_VIDEO_HOLD:-0}" = 1 ]; then
		CFLAGS="$CFLAGS -DSHIM_VIDEO_HOLD=1"
	fi
fi
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	(cd out && rm -rf rd && mkdir rd && cd rd && find . | cpio -o -H newc --quiet | gzip -9 > ../ramdisk.bin)
fi
mkbootimg --header_version 2 --kernel out/Image --ramdisk out/ramdisk.bin \
	--dtb "${DTB:-../dtb/boot_unpacked/dtb}" --base 0 --kernel_offset 0x8000 --ramdisk_offset 0x1000000 \
	--tags_offset 0x100 --dtb_offset 0x1f00000 --pagesize 4096 \
	--os_version 17.0.0 --os_patch_level 2035-06 \
	--cmdline "console=null" -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
