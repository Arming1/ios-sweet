# venus（Xiaomi Mi 11, SM8350）移植说明

`apply-venus-port.py` 在这个工作副本上做的改动，数值依据见上层项目的 `notes/patch-analysis.md`。

## 当前状态（2026-10-25）
- XNU（xnu-8796.141.3 + 补丁 0001..0098）在 Mi 11 上启动，运行未修改的 iOS 16.7.16（20H392）iPhone X 用户空间。锁屏约 1.5 分钟后出现，可滑动解锁。首次启动约 14 分钟（一次性数据迁移）。
- 设置、关于本机、iPhone 存储空间可用（0096、0097）。
- 触摸可用（多点触控、点按、滚动、底部上滑手势）。电源、音量加、音量减可用（0092、0093）。
- 全屏原生 1440x3200（0091）。
- 8 核 AMP（0087）：0-3 为 A55（E 簇），4-7 为 3×A78 + X1（P 簇），调度器 Edge。
- 调频（0094，`sweetcpufreq=15,10,10`，15 s 后切换）后实测 A55 1802 MHz、A78 1880 MHz、X1 2262 MHz（ABL 留下的是 A55 1494、A78 709、X1 958 MHz）。
- USB 实时日志改为 RNDIS（`sweetusb=3`，0089）。
- 未完成：GPU、Wi-Fi、蓝牙、音频、相机、蜂窝网络、实时时钟（显示 1969 年 12 月 31 日，星期三）、电池电量（静态）、背光（锁屏只是画黑）、FairPlay（`fairplayd.H2` 因为没有 FairPlayIOKit 驱动，启动后立刻 exit(0)）。`driverkitd` 每次启动崩溃一次。渲染靠 CPU，UFS I/O 为轮询方式。
- 已验证：ABL 交接时 INTF_1 autorefresh 开着，只写帧缓冲即可稳定显示；绝不能再手动写 CTL_START（会让 DSC 数据流失步、花屏）。

## 生成 payload（需要 macOS 构建出的内核）
    python3 tools/mkadt.py shim/out/venus.adt --highmem --canvas 1440x3200 --subtype 2778
    python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/venus.adt shim/out/payload.bin \
        --cmdline '<下面两个变体之一>'
    # 打包成 Android boot image header v3：kernel = shim 的 Image，ramdisk = payload.bin
    # （不要用 shim/build.sh 的 header v2 + DTB 方式，venus 的 ABL 会拒绝）
    fastboot boot venus-ios.img     # 只用 fb-guard 包装，绝不 flash

- `mkadt.py` 默认就是 E/P 两簇（0087）；`--smp` 才恢复单一 SMP 簇，这里不用。`--highmem` 仍然需要。
- `--canvas WxH`：iOS 渲染的画布尺寸；`--subtype N`：/product 的 artwork-device-subtype（全屏用 2778）。

### 变体一：完整 iOS 启动（写 iOS 根文件系统）
前提：`userdata` 已刷入 `tools/mkiosroot.sh` 生成的 iOS 根镜像（`fastboot -S 512M flash userdata ios16-root.simg`，约 5–7 分钟，会清除 Android 用户数据）。

    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweettouch=1 sweetkeys=1 sweetusb=3 sweetnoexec=bluetoothd sweetfb_native=1 ufswait=1 ufscache=1 sweetcpufreq=15,10,10'

### 变体二：只读测试（不挂载 iOS 根，不写 userdata）
与变体一相同，但 `ufsrw=0 rootrw=0`，并加上 `sweetlog=0 ufsroot=persist`。根文件系统挂载会按设计失败；USB、触摸、按键、调频等内核线程照常运行。
只做内核测试时必须保留 `rd=ufsroot0`：没有它，XNU 会一直等待 Apple_HFS IOMedia，UFS、USB、按键、触摸、调频等 sweet 驱动都不会启动（2026-10-25 实测）。

    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=0 rootrw=0 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweettouch=1 sweetkeys=1 sweetusb=3 sweetnoexec=bluetoothd sweetfb_native=1 ufswait=1 ufscache=1 sweetcpufreq=15,10,10 sweetlog=0 ufsroot=persist'

### 启动参数（venus / sweet 扩展）
| 参数 | 补丁 | 说明 |
| --- | --- | --- |
| `sweetfb_native=1` | 0091 | 按面板真实尺寸呈现（全屏）。 |
| `sweetfb_size=WxH` | 0091 | 指定呈现尺寸，例如 `1284x2778`（偶数，不大于面板）。 |
| `sweetunlink=/path` | 0091 | launchd 启动前删除 iOS 根里的一个文件。一次性使用：删除了 PodcastsUsagePlugin 和 AirFair.syncBundle/AirFair，不在变体一里。 |
| `sweetkeys` | 0060, 0092, 0093 | 按键轮询，默认开；`sweetkeys=0` 关。 |
| `sweetusb` | 0089 | 0 关，1 只报告状态（默认），2 CDC-ECM，3 RNDIS。 |
| `sweetnoexec=a,b` | 0088, 0090 | 拒绝执行列出的程序（EPERM，带退出原因）。launchd 约每 10 s 重试一次。 |
| `ufswait=1 ufscache=1` | 0069（上游） | 等待时睡眠而不是自旋；缓存维护更省。 |
| `sweetcpufreq=a,b,c` | 0094 | 按域（D0/D1/D2）写 PERF_STATE 档位；255 或留空 = 不变。只写 PERF_STATE，只有给出该参数时才写，15 s 后切换。 |
| `sweetbusy` | 0095 | 记录 busy 的 IOService；默认开，`sweetbusy=0` 关。 |
| `sweetsample=names` | 0054 | 线程采样器：首次 90 s 后，之后每 120 s 打印一次。 |
| `sweetscan=names` | 0051, 0053, 0098 | 进程中止时打印堆字符串；0098 还记录这些进程的退出状态和回溯。 |

改变显示尺寸后要删一次 MobileGestalt 缓存（见「与 sweet 不同的值」）。

## USB 实时日志（`sweetusb=3`，RNDIS）
- 设备 `1209:0002`，Windows 自带 RNDIS 驱动（兼容 ID `USB\Class_EF&SubClass_04&Prot_01`），不需要 usbipd。
- 首次：以管理员身份把新适配器设为 `192.168.7.1/24`。
- 内核日志以 UDP 端口 `5140` 发送，来源 `192.168.7.2`。
- 只读 iOS 根加用户空间时 gadget 起不来（设备管理器代码 10）。内核测试（`ufsroot=persist`）和可写 iOS 根的启动正常。

## 与 sweet 不同的值
| 项目 | sweet | venus |
| --- | --- | --- |
| 面板 | 1080x2400 | 1440x3200（K2，DSC 命令模式） |
| 显示尺寸 | 1080x2400 | ADT canvas 1440x3200，subtype 2778（`mkadt.py --canvas --subtype`） |
| 帧缓冲 | 0x9c000000 / 0x1700000 | 0xe2a00000 / 0x2300000（实测 VIG0 扫描地址；32MB 块基址 0xe2000000，块内偏移 0xa00000；基础 DTB 写的 0xea600000 不对） |
| 虚拟 / 物理定时器 | INTID 19 / 18 | INTID 27 / 30 |
| IMEM restart_reason | 0x146aa65c | 0x146bf65c |
| 看门狗 | - | 0x17c10000：shim 连读都会卡死总线；它没有被 ABL 武装，所以 V60 式关闭（VENUS_WDT_DISABLE）默认关、从不访问 |
| CPU | A55x6 + A76x2 | A55x4（E 簇）+ A78x3 + X1x1（P 簇），MPIDR 0x000-0x700 |
| 在线 CPU 数 | 未记录 | 8（cpus=8，PSCI CPU_ON；E/P 两簇 AMP，不是单一 SMP 调度域） |
| 按键 | 音量加：PM6150L SID 4（0092 原注释） | 电源：PMK8350 PON_HLOS RT_STS 0x1310 bit 7；音量减：同一寄存器 bit 6（SID 0）；音量加：PM8350 GPIO6，SID 1，0x8d10，低有效 |
| 调频 | 未记录 | EPSS cpufreq-hw 0x18591000 / 0x18592000 / 0x18593000：EN +0x0，DOMAIN_STATE +0x20（低 8 位 × 19.2 MHz），LUT +0x100（每行 4 字节），PERF_STATE +0x320；大核域在 shim 阶段 EN=2、STATE=0，因为核未上电 |
| 触摸 | 未记录 | ST FTS（FTM5），QUP1 SE3 SPI 0x00a8c000（CS0，mode 0，4.8 MHz）；复位 GPIO22，中断 GPIO23（低有效），AVDD GPIO132；SPI 引脚 GPIO48-51（qup11）；VDD 为 PM8350C L2 1.8 V（RPMh "ldoc2"）；芯片 ID 36 39；坐标 0..1439 x 0..3199 |
| USB | 未记录 | DWC_usb31 v1.10a @0x0a600000（GSNPSID 0x33313130），QSCRATCH 0x0a6f8800，GDSC 0x10f004；HS femto PHY 0x088e3000（从未访问） |

相同的值：GICD 0x17a00000、GICR 0x17a60000（8 个 frame）、UFS 0x1d84000、SPMI arbiter 0x0c440000、--highmem 内存 0x1_00000000 + 0xfcc00000。

- RPMh apps RSC DRV 2 在 0x18220000，TCS 偏移 0xd00，布局与 sweet 相同。
- PM8350C 的 LDO 只能经 RPMh 控制，在 SPMI 上看不到。
- 改变显示尺寸后，MobileGestalt 缓存要删一次：`/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`，用 `sweetunlink` 删除。

## venus 补丁（0082–0098）
- 0082 venus-no-physical-timer：跳过 CNTP_CTL_EL0 写入（SM8350 上 EL1 物理定时器被 trap）。
- 0083 venus-usb：USB 探测使用 venus 的 GCC 时钟偏移（gcc-lahaina），不读 QUSB2 PHY（0x088e2000 不存在），DWC_usb31 v1.10a，gadget 保持高速（USB 2.0）。
- 0084 venus-usb-reset：gadget 像 Linux dwc3 一样处理总线复位和断开（结束传输、取消配置、重新挂起 ep0 SETUP），因为 Windows 主机（usbipd）会循环切换端口。
- 0085 venus-fts-touch：sweet_touch.c 重写为 ST FTS（FTM5）触摸芯片，经 GENI SPI 通信；sweet_rpmh.c 也接受 "ldoc2" 1.8 V。
- 0086 venus-touch-panel-size：SweetHIDTouch 按 1440x3200 限幅和边缘判断（原为 1080x2400）。
- 0087 venus-amp-clusters：ADT cluster-type E/P，MAX_CPU_CLUSTERS 2，`__ARM_AMP__`；mkadt 默认 E/P，`--smp` 恢复单一簇。
- 0088 venus-exec-deny：`sweetnoexec`，拒绝执行的程序返回 EPERM。
- 0089 venus-usb-rndis：`sweetusb=3`，RNDIS gadget。
- 0090 venus-exec-deny-reason：修复 0088 的 panic：每个错误都需要 exit reason。
- 0091 venus-display-size：`sweetfb_size`；`sweet_boot_unlink`（`sweetunlink`）在 load_init_program 之前删除一个文件。
- 0092 venus-keys：PMK8350 PON（电源、音量减）与 PM8350 GPIO6（音量加）按键。
- 0093 venus-pon-key-bits：电源为 bit 7，音量减为 bit 6；记录原始 PON 变化。
- 0094 venus-cpufreq：EPSS 报告、逐核基准、`sweetcpufreq`。
- 0095 venus-iokit-busy-dump：SweetBusy（`sweetbusy`）。
- 0096 venus-iokit-quiesce：移植自 MakrSas/ios-topaz 0128。不再为 IOKit 守护进程在注册表根上保持 busy，关于本机不再卡住。
- 0097 venus-sandbox-extensions：沙盒扩展用假 token（issue / consume 都成功），tccd 因此能授予 kTCCServicePhotos，assetsd 不再中止。
- 0098 venus-exit-log：`sweetscan` 名单中进程的 EXIT 日志（退出状态，以及当前线程的回溯）。

## 已解决问题（根因）
- 关于本机卡住：根注册表 busy=1 一直不变（0095 日志），`SCNetworkInterfaceCopyAllWithPreferences` 一直等待 IOKit 静止直到超时。0096 修复。
- iPhone 存储空间：
  - (a) PodcastsUsagePlugin 的 Swift trap：已删除该插件。
  - (b) assetsd 报 "requires TCCPhotosService access"：0097 修复。
  - (c) atc / STStorageMediaMonitor 在 AirFair.syncBundle 中等 fairplayd 永远不返回：没有 FairPlayIOKit，fairplayd 在报到前就 exit(0)。已删除 AirFair。

## shim 实验（EXPERIMENT=10..17）
- 10 = U1：USB（GCC / DWC3 / QSCRATCH）。
- 11 = T1：触摸（TLMM GPIO 22/23/132，QUP1 时钟，SE3）。
- 12 = T2：QUP1 时钟投票 + SE3 固件版本 + 经 SPMI 读取 PMIC L2。
- 13 = C1：cpufreq（只读）。
- 14 = C2：cpufreq PERF 写入测试。
- 15 = V0：framebuffer 速度。MMU 关：全屏填充 448 ms；MMU 开，framebuffer Normal-NC：6.2 ms。
- 16 = U2：USB bulk 基准，35 MB/s。
- 17 = USB 流接收端：等待 10 分钟，然后保持（见下文「Live video over USB」）。

## Custom boot logo (optional)
The shim can draw an image while it hands over to XNU.

    SHIM_LOGO_IMAGE=/path/to/logo.png shim/build.sh

- `SHIM_LOGO_MAX` (optional): limit for the longer side in pixels, default 400. The logo is drawn at 3x, centred, from row 1000.
- Requires Python 3 with Pillow.
- The image and the generated header stay local: `shim/out` is git-ignored. No logo is shipped with the repo.
- Tip: flat-colour logos compress far better than photos.

## Silent boot video (optional)
The shim can play a silent clip before it hands over to XNU.

    SHIM_VIDEO_FILE=/path/clip.mp4 [SHIM_VIDEO_FPS=60] [SHIM_VIDEO_HOLD=1] shim/build.sh

- Needs ffmpeg and Python 3 with Pillow. 360x640 by default (`--w`, `--h`), drawn at 3x (1080x1920 on the panel) and centred.
- One global 256-colour palette, plus per-frame delta coding.
- The shim image must stay below 16 MB (the ramdisk is at 0x1000000), so keep clips short.
- `start.S` uses adrp + add for symbol addresses, which the large `.rodata` of the video needs.
- Measured at 30 and 60 fps with 0 late frames (worst frame about 6.9 ms).
- Playback stats are printed after the video. The default build then turns the MMU and caches off and boots XNU. `SHIM_VIDEO_HOLD=1` holds on the last frame instead (hold Power 15s to reset).
- The clip stays local: `shim/out` is git-ignored.

## Live video over USB (optional)
    SHIM_USBSTREAM=1 [SHIM_USBSTREAM_WAIT_MS=10000] shim/build.sh

- The shim enumerates as WinUSB device `1209:0003` (BOS + MS OS 2.0 descriptors). Windows binds WinUSB itself, so no driver install is needed.
- Protocol (`usbstream.c`, little-endian 16-byte headers): `SWVS` start (size, format RGB565 or PAL8), `SWVP` palette (PAL8), `SWVF` frame, `SWVE` end (hold on the last frame, or boot XNU).
- Frames are drawn row by row as they arrive, at an integer scale, centred. The framebuffer is mapped Normal non-cacheable (write-combining). Bulk RX buffers are cacheable, maintained with `dc civac` / `dc ivac`; the event buffer and TRBs sit in an NC window.
- Host: `tools/win-usbstream.py --play clip.mp4 --fps 60 --size 360x640 [--rotate 90|270] [--format pal8] [--loop N] [--end hold|boot]`, plus `--testpattern` and `--bench SECONDS`. `--play` needs ffmpeg (env `FFMPEG`, else PATH); the other modes do not.
- Measured: bulk 35 MB/s (USB 2.0 HS). 360x640 RGB565 at 60.0 fps (26.4 MB/s), 0 late frames after ffmpeg start-up.
- Limit: width x height x 2 x fps below about 30 MB/s. 1280x720 RGB565 reaches only about 19–20 fps.
- A still image is one frame. After SWVE boot, the shim stops the controller (halted, device mode) and XNU's own driver takes it over (live log verified).

## 未验证
- PSHOLD 0x0c264000：venus DT 里没有，沿用 sweet。
- sweetlog 环在 iOS 根文件系统上不写入（原因不明）。
- USB SuperSpeed 在 shim 中未尝试（fastboot 运行在 SS，说明硬件支持）。
