# ios-sweet: venus (Xiaomi Mi 11) port

An experimental port of the [ios-sweet](https://github.com/dipa-studio/ios-sweet) work to the Xiaomi Mi 11 (codename `venus`, Qualcomm SM8350 / Snapdragon 888, 1440x3200 DSC command-mode panel).

Target userland, as upstream: unmodified iOS 16.7.16 (20H392) of the iPhone X. Nothing from Apple is distributed in this repository.

The upstream README, which describes the Redmi Note 10 Pro (`sweet`), is kept unchanged as [README.upstream.md](README.upstream.md).

## 简体中文摘要

- 这是 [ios-sweet](https://github.com/dipa-studio/ios-sweet) 向小米 Mi 11（代号 venus，骁龙 888）移植的实验性分支。
- 目标用户空间与上游相同：未修改的 iOS 16.7.16（20H392，iPhone X 版本）。本仓库不分发任何 Apple 文件。
- 当前状态（2026-10-25）：XNU 内核（xnu-8796.141.3 + 补丁 0001..0098）已在 Mi 11 上启动，运行未修改的 iOS 16.7.16 用户空间，可以到达锁屏、滑动解锁、进入主屏幕和设置。
- 已可用：触摸、按键（电源、音量加、音量减）、8 核 AMP（A55 为 E 类，A78/X1 为 P 类）加调频、全屏 1440x3200、设置 / 关于本机 / 存储空间、USB 实时日志（RNDIS）、shim 开机 logo / 视频 / USB 实时推流。
- 未完成：GPU、Wi-Fi、蓝牙、音频、相机、蜂窝网络、实时时钟、电池电量、背光、FairPlay。渲染由 CPU 完成，速度慢。
- 启动方式：引导镜像只用 `fastboot boot` 启动，从不刷写。iOS 根文件系统（本地构建的 HFS+ 镜像）刷写到 `userdata` 分区，会清除 Android 用户数据，镜像不发布。需要已解锁 Bootloader 的 Mi 11；不要重新上锁。
- 上游 README 保存在 [README.upstream.md](README.upstream.md)，venus 专用改动见 [VENUS-PORT.md](VENUS-PORT.md)。

## Status (2026-10-25)

Confirmed on the phone:

- The shim runs.
- PSCI `SYSTEM_RESET` works.
- The shim draws its console (dark grey background, text) into the scanout buffer.
- The UFS host answers, and the shim reads the GPT (read-only).
- XNU (xnu-8796.141.3 + patches 0001..0098) boots on the Mi 11 and runs the unmodified iOS 16.7.16 (20H392) iPhone X userland.
- The lock screen appears about 1.5 minutes after boot. The first boot took about 14 minutes (one-time data migration).
- Swipe-up unlock works. The home screen shows Photos, App Store and Settings; the dock shows Phone, Safari and Messages.
- Touch works: multi-touch events, taps, scrolling, and the home gesture from the bottom edge.
- Buttons work: power, volume up and volume down (0092, 0093). Long-pressing power brings up Siri (normal iOS behaviour). Holding power for about 10 s triggers the PMIC hard reset.
- Full screen: iOS renders natively at 1440x3200 (0091, boot-arg `sweetfb_native=1`).
- All 8 CPUs run (`cpus=8`). They are started with PSCI `CPU_ON`. Cores 0-3 (A55) are cluster type E, and cores 4-7 (3x A78 + X1) are cluster type P (patch 0087; the mkadt default; scheduler Edge).
- CPU frequency (0094, boot-arg `sweetcpufreq=15,10,10`): measured per core are A55 1802 MHz, A78 1880 MHz and X1 2262 MHz. The switch happens 15 s after boot. Only PERF_STATE is written.
- Settings > General > About no longer stalls (0096, ported from MakrSas/ios-topaz patch 0128). The IOKit registry root had stayed busy, waiting for the IOKit daemon that does not run here.
- Settings > General > iPhone Storage works (0097). Two Apple bundles were removed from the local iOS root with `sweetunlink` (patch 0091): PodcastsUsagePlugin and `AirFair.syncBundle`.
- Live kernel log over USB uses RNDIS (boot-arg `sweetusb=3`, patch 0089).

Known problems:

- Slow: rendering is done by the CPU (no GPU driver). UFS I/O is polled. `ufswait=1 ufscache=1` make the UFS waits faster (upstream 0069).
- Locking the phone only paints the screen black. There is no backlight control.
- FairPlay is unavailable. `fairplayd.H2` exits at once (exit status 0), because Apple's FairPlayIOKit driver does not exist here.
- `bluetoothd` is not started (`sweetnoexec=bluetoothd`, 0088/0090).
- `driverkitd` crashes once per boot.
- The persistent log ring at the end of `userdata` is not written when booting the iOS root (cause unknown).

Not yet:

- GPU, Wi-Fi, Bluetooth, audio, camera, cellular.
- Backlight control and FairPlay.
- Real-time clock: the date shows Wednesday, December 31 1969.
- Battery level is static.

Next steps:

1. GPU (Adreno 660). The HackiOS redwood driver is the closest reference.
2. A daily command line without the diagnostics.
3. An earlier cpufreq switch (now 15 s after boot).
4. Backlight control.

## How it boots

Only `fastboot boot` is used for the boot image. The boot image is never flashed.

- Boot image: Android boot image header v3. The kernel is the shim `Image`; the ramdisk is the payload (kernel + venus Apple Device Tree + command line).
- The venus bootloader accepts an Android boot image header v3 with the shim as the kernel. The device tree then comes from the phone's own `vendor_boot`.
- A header v1 image with an appended DTB is rejected with `Failed to load/authenticate boot image: Load Error`. The bootloader then marks the current slot unbootable. Repair it with `fastboot set_active <slot>`.
- Root filesystem: a 16 GiB HFS+ image built locally from the official iPhone X IPSW with `tools/mkiosroot.sh` (Linux or WSL, `hfsplus` module). It is flashed to the `userdata` partition:

  ```
  fastboot -S 512M flash userdata ios16-root.simg
  ```

  This takes about 5-7 minutes and WIPES Android user data. The image is never published (it contains Apple content).

- Recommended kernel command line (v20, on the developer's phone):

  ```
  debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweettouch=1 sweetkeys=1 sweetusb=3 sweetnoexec=bluetoothd sweetfb_native=1 ufswait=1 ufscache=1 sweetcpufreq=15,10,10
  ```

  The diagnostics (`sweetscan`, `sweetsample`) are omitted here. `sweetunlink=<path>` (patch 0091) deletes a file in the iOS root. It is a one-off and is not in this line.

- Read-only test variant: the same command line with `ufsrw=0 rootrw=0` instead of `ufsrw=1 rootrw=1`, plus `sweetlog=0 ufsroot=persist`. It does not mount the iOS root and does not write `userdata`. The root mount then fails on purpose; kernel threads such as USB and touch still run.

## Display

- Scanout buffer `0xE2A00000` (measured). The base device tree's `splash_region` (`0xEA600000`) is not what the bootloader scans under `fastboot boot`.
- 1440x3200, linear ARGB, stride 5760.
- Two pipes (VIG0 and VIG1, source split at 720 px each) read the same buffer.
- The bootloader leaves INTF_1 autorefresh on, so writing the buffer is enough.
- Never write `CTL_START` while autorefresh is on. It desynchronises the DSC stream and garbles the panel.
- iOS renders natively at 1440x3200 with `sweetfb_native=1` (0091). The ADT canvas is 1440x3200 (`tools/mkadt.py --canvas`).

## Key hardware facts

| Item | Value on venus |
| --- | --- |
| SoC | Qualcomm SM8350 (Snapdragon 888) |
| CPUs | 4x Cortex-A55, 3x Cortex-A78, 1x Cortex-X1; MPIDR `0x000`-`0x700` (Aff1 = core); all 8 online (`cpus=8`) |
| CPU clusters | Cores 0-3 (A55) cluster type E; cores 4-7 (3x A78 + X1) cluster type P (patch 0087); scheduler Edge |
| CPU clock | EPSS cpufreq-hw at `0x18591000`, `0x18592000`, `0x18593000`. ABL leaves A78 at 709 MHz, X1 at 958 MHz, A55 at 1494 MHz. Only PERF_STATE is written (0094) |
| Buttons | Power = PMK8350 PON_HLOS RT_STS bit 7; volume down = bit 6 (SID0 `0x1310`); volume up = PM8350 GPIO6 (SID1 `0x8d10`), active low |
| GICD / GICR | `0x17a00000` / `0x17a60000` (8 GICR frames) |
| Virtual / physical timer INTID | 27 / 30 |
| UFS host | `0x1d84000` |
| IMEM restart_reason | `0x146bf65c` |
| Apps watchdog | `0x17c10000` |
| Kernel RAM (`--highmem`) | `0x1_00000000` + `0xfcc00000` |
| Framebuffer | `0xE2A00000`, 1440x3200, stride 5760 |
| USB controller | Synopsys DWC_usb31 v1.10a at `0x0a600000` (GSNPSID `0x33313130`); QSCRATCH `0x0a6f8800`; GDSC `0x10f004` |
| USB HS PHY | femto PHY at `0x088e3000` (never touched). ABL leaves the controller powered, clocked, in device mode, and halted |
| Touch | ST FTS (FTM5) on QUP1 SE3 SPI at `0x00a8c000` (CS0, mode 0, 4.8 MHz from XO; SE firmware = SPI, revision `0x102`). Chip id `36 39`; reports 0..1439 x 0..3199. ABL leaves the touch path off |
| Touch GPIOs | reset GPIO22, IRQ GPIO23 (active low), AVDD enable GPIO132, SPI pins GPIO48-51 (function `qup11`) |
| Touch power | VDD = PM8350C L2 1.8 V, supplied via RPMh (`ldoc2`) |
| RPMh | apps RSC DRV 2 at `0x18220000`, TCS offset `0xd00` (same layout as sweet) |
| SPMI | arbiter v5 at `0x0c440000` (same address as sweet). PM8350C LDOs are RPMh-only and not visible on SPMI |

The apps watchdog block at `0x17c10000` is not accessible from the shim: even a read hangs. It is not armed, so the V60-style disable (`VENUS_WDT_DISABLE`) is off by default.

## Debugging method

Some checks need no screen or USB output.

- **Heartbeat shims** reboot at a checkpoint, so the last checkpoint reached can be seen.
- **Timed heartbeat** waits 2+2n seconds before rebooting. A host can read a 4-bit value from the boot time.

Builds come from `.github/workflows/shim.yml`, with `HEARTBEAT=N` and `EXPERIMENT=N` variants.

**Live kernel log over USB (`sweetusb=3`, RNDIS).** The phone appears as USB device `1209:0002`. Windows uses its inbox RNDIS driver, so no usbipd is needed.

1. Once, give the new adapter `192.168.7.1/24`.
2. The kernel console arrives as UDP on port `5140`, from the phone at `192.168.7.2`.

**Live kernel log over USB (`sweetusb=2`, CDC-ECM).** The phone appears as USB device `1209:0001` "Sweet iOS (USB Ethernet)". It is a CDC-ECM Ethernet gadget: the phone is `192.168.7.2`, the host is `192.168.7.1`, and the kernel console streams as UDP to `192.168.7.1:5140`. Windows has no CDC-ECM driver, so the device is passed to WSL2 with usbipd-win:

1. Install usbipd-win: `winget install --exact dorssel.usbipd-win`.
2. Once, as administrator: `usbipd policy add --effect Allow --operation AutoBind --hardware-id 1209:0001`.
3. When the device appears, attach it: `usbipd attach --wsl --busid <id> --auto-attach`.
4. In WSL2 (kernel modules `vhci-hcd` and `cdc_ether`), give the interface `192.168.7.1/24` and receive the UDP stream with `socat`.

On Linux hosts, `tools/sweet-usbnet.sh` does steps 3 and 4.

**Diagnostics (boot-args).**

- `sweetsample`: thread sampler.
- `sweetscan`: heap strings of aborting processes (0051/0053). 0098 logs the exit status and backtrace of `sweetscan` processes.
- `sweetbusy` (0095, SweetBusy): logs busy IOServices.

## Safety

- Experimental. Do not use it as a daily phone.
- Boot images are started only with `fastboot boot`. Never flash `boot`, `vbmeta` or `dtbo`.
- Only `userdata` is flashed, with the iOS root. This erases Android's `userdata` partition. Kernels that write the root need `ufsrw=1 rootrw=1`; all other experiments use read-only boot arguments.
- Never relock the bootloader.
- Requires an already-unlocked Mi 11.

## Building

### Kernel (xnu)

`.github/workflows/xnu.yml` builds xnu-8796.141.3 on GitHub's macOS runner, with all 98 patches in `xnu/patches` applied (0001-0098). The venus changes are patches 0082-0098. See [VENUS-PORT.md](VENUS-PORT.md).

### Shim

`.github/workflows/shim.yml` builds the shim `Image` (plus the `HEARTBEAT` / `EXPERIMENT` variants). The `fastboot boot` image is packed separately as an Android boot image header v3: the shim `Image` is the kernel, and the ramdisk is either empty (diagnostics only) or the XNU payload made by `tools/mkadt.py` and `tools/mkpayload.py`. The shim finds the payload behind the phone's own vendor ramdisk.

The shim experiments are probes, built with `EXPERIMENT=N`:

- 10 = U1 USB (GCC, DWC3, QSCRATCH).
- 11 = T1 touch (TLMM GPIO 22/23/132, QUP1 clocks, SE3).
- 12 = T2 (QUP1 clock vote, SE3 firmware revision, PMIC L2 over SPMI).
- 13 = C1 cpufreq (read-only).
- 14 = C2 cpufreq (write test).
- 15 = V0 framebuffer speed.
- 16 = U2 USB bulk benchmark.
- 17 = USB stream test (waits 10 min, then holds).

Optional shim extras are built locally with `shim/build.sh`. Nothing ships in the repository. The image and clip stay local, and `shim/out` is git-ignored. Details are in [VENUS-PORT.md](VENUS-PORT.md).

- Boot logo: `SHIM_LOGO_IMAGE=/path/logo.png shim/build.sh` (`tools/mklogo.py`).
- Silent boot video: `SHIM_VIDEO_FILE=clip.mp4 shim/build.sh` (`tools/mkvideo.py`). The shim must stay below 16 MB.
- Live video over USB: `SHIM_USBSTREAM=1 shim/build.sh`. The shim becomes a WinUSB device `1209:0003` and draws frames streamed by `tools/win-usbstream.py`.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) |
| `xnu/patches/` | The 98 XNU patches (0001-0098), applied in order by CI |
| `tools/` | Host tools, e.g. `mkadt.py` (Apple Device Tree), `mkpayload.py` (kernel + ADT payload), `mkiosroot.sh` (iOS root image), `sweet-usbnet.sh` (USB Ethernet on Linux), `mklogo.py` (boot logo), `mkvideo.py` (boot video) and `win-usbstream.py` (live video over USB) |
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
- [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz): patch 0128 (ported as 0096), and the cpufreq and sandbox references.
- [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) (HackiOS): GPU reference (redwood Adreno driver), not used yet.

iOS itself is Apple's and is not distributed here.
