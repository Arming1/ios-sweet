# ios-sweet: venus (Xiaomi Mi 11) port

An experimental port of the [ios-sweet](https://github.com/dipa-studio/ios-sweet) work to the Xiaomi Mi 11 (codename `venus`, Qualcomm SM8350 / Snapdragon 888, 1440x3200 DSC command-mode panel).

Target userland, as upstream: unmodified iOS 16.7.16 (20H392) of the iPhone X. Nothing from Apple is distributed in this repository.

The upstream README, which describes the Redmi Note 10 Pro (`sweet`), is kept unchanged as [README.upstream.md](README.upstream.md).

## 简体中文摘要

- 这是 [ios-sweet](https://github.com/dipa-studio/ios-sweet) 向小米 Mi 11（代号 venus，骁龙 888）移植的实验性分支。
- 目标用户空间与上游相同：未修改的 iOS 16.7.16（20H392，iPhone X 版本）。本仓库不分发任何 Apple 文件。
- 当前状态：自制引导 shim 可以在手机上运行并在屏幕上输出控制台；XNU 内核还没有在手机上启动。
- 只使用 `fastboot boot`，从不刷写分区。需要已解锁 Bootloader 的 Mi 11；不要重新上锁。
- 上游 README 保存在 [README.upstream.md](README.upstream.md)，venus 专用改动见 [VENUS-PORT.md](VENUS-PORT.md)。

## Status (2026-10-24)

Confirmed on the phone:

- The shim runs.
- PSCI `SYSTEM_RESET` works.
- The shim draws its console (dark grey background, text) into the scanout buffer.
- The UFS host answers, and the shim reads the GPT (read-only).

Not yet:

- The XNU kernel has not booted on the phone.
- iOS has not run on the Mi 11.
- No touch, buttons, USB, battery, Wi-Fi, audio or GPU work.

Next steps:

1. Boot the XNU payload (shim + kernel + Apple Device Tree) and get the kernel log on screen.
2. Mount the root filesystem on UFS.

## How it boots

Only `fastboot boot` is used. The image is never flashed.

- The venus bootloader accepts an Android boot image header v3 with the shim as the kernel. The device tree then comes from the phone's own `vendor_boot`.
- A header v1 image with an appended DTB is rejected with `Failed to load/authenticate boot image: Load Error`. The bootloader then marks the current slot unbootable. Repair it with `fastboot set_active <slot>`.

## Display

- Scanout buffer `0xE2A00000` (measured). The base device tree's `splash_region` (`0xEA600000`) is not what the bootloader scans under `fastboot boot`.
- 1440x3200, linear ARGB, stride 5760.
- Two pipes (VIG0 and VIG1, source split at 720 px each) read the same buffer.
- The bootloader leaves INTF_1 autorefresh on, so writing the buffer is enough.
- Never write `CTL_START` while autorefresh is on. It desynchronises the DSC stream and garbles the panel.

## Key hardware facts

| Item | Value on venus |
| --- | --- |
| SoC | Qualcomm SM8350 (Snapdragon 888) |
| CPUs | 4x Cortex-A55, 3x Cortex-A78, 1x Cortex-X1; MPIDR `0x000`-`0x700` |
| GICD / GICR | `0x17a00000` / `0x17a60000` |
| Virtual / physical timer INTID | 27 / 30 |
| UFS host | `0x1d84000` |
| IMEM restart_reason | `0x146bf65c` |
| Apps watchdog | `0x17c10000` |
| Kernel RAM (`--highmem`) | `0x1_00000000` + `0xfcc00000` |
| Framebuffer | `0xE2A00000`, 1440x3200, stride 5760 |

The apps watchdog block at `0x17c10000` is not accessible from the shim: even a read hangs. It is not armed, so the V60-style disable (`VENUS_WDT_DISABLE`) is off by default.

## Debugging method

Some checks need no screen or USB output.

- **Heartbeat shims** reboot at a checkpoint, so the last checkpoint reached can be seen.
- **Timed heartbeat** waits 2+2n seconds before rebooting. A host can read a 4-bit value from the boot time.

Builds come from `.github/workflows/shim.yml`, with `HEARTBEAT=N` and `EXPERIMENT=N` variants.

## Safety

- Experimental. Do not use it as a daily phone.
- Never relock the bootloader.
- A full install would erase Android's `userdata` partition. This has not been done.
- Requires an already-unlocked Mi 11.

## Building

### Kernel (xnu)

`.github/workflows/xnu.yml` builds xnu-8796.141.3 on GitHub's macOS runner, with all 81 patches in `xnu/patches` applied. The venus changes only change numbers and addresses in existing patches. See [VENUS-PORT.md](VENUS-PORT.md).

### Shim

`.github/workflows/shim.yml` builds the shim `Image` (plus the `HEARTBEAT` / `EXPERIMENT` variants). The `fastboot boot` image is packed separately as an Android boot image header v3: the shim `Image` is the kernel, and the ramdisk is either empty (diagnostics only) or the XNU payload made by `tools/mkadt.py` and `tools/mkpayload.py`. The shim finds the payload behind the phone's own vendor ramdisk.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) |
| `xnu/patches/` | The 81 XNU patches, applied in order by CI |
| `tools/` | Host tools, e.g. `mkadt.py` (Apple Device Tree) and `mkpayload.py` (kernel + ADT payload) |
| `.github/workflows/xnu.yml` | Kernel build |
| `.github/workflows/shim.yml` | Shim build, including the `HEARTBEAT` and `EXPERIMENT` variants |
| `VENUS-PORT.md` | venus-specific changes and their verification status |
| `README.upstream.md` | The upstream ios-sweet README (Redmi Note 10 Pro), unchanged |

## Credits

This fork builds on [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet). All upstream credits apply; see [README.upstream.md](README.upstream.md).

Other references used:

- [ch1tmdgus/ios-v60](https://github.com/ch1tmdgus/ios-v60) (LG V60 port) for the per-device change pattern.
- [xiaomi-haydn-dev/UniXNU-haydn](https://github.com/xiaomi-haydn-dev/UniXNU-haydn) notes (same SoC, Redmi K40 Pro) for the SM8350 hardware.
- Xiaomi's venus kernel source, [MiCode/Xiaomi_Kernel_OpenSource](https://github.com/MiCode/Xiaomi_Kernel_OpenSource), branch `venus-r-oss`, for display registers.

iOS itself is Apple's and is not distributed here.
