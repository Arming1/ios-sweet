# venus（Xiaomi Mi 11, SM8350）移植说明

`apply-venus-port.py` 在这个工作副本上做的改动，数值依据见上层项目的 `notes/patch-analysis.md`。

## 当前状态（2026-10-24）
- XNU（xnu-8796.141.3 + 补丁 0001..0086）在 Mi 11 上启动，运行未修改的 iOS 16.7.16（20H392）iPhone X 用户空间。锁屏约 1.5 分钟后出现，可滑动解锁，设置可用。首次启动约 14 分钟（一次性数据迁移）。
- 触摸可用（多点触控、点按、滚动、底部上滑手势）。
- 8 个 CPU 全部运行（cpus=8），由 PSCI CPU_ON 启动，调度器视为单一 SMP 集群。
- 内核日志可通过 USB（CDC-ECM 以太网 gadget，UDP）实时查看。
- 未完成：按键（电源、音量）、Wi-Fi、蓝牙、音频、相机、蜂窝网络、GPU。实时时钟显示 1969 年 12 月 31 日（星期三），电池电量为静态值。渲染由 CPU 完成，UFS I/O 为轮询方式。
- 已验证：ABL 交接时 INTF_1 autorefresh 开着，只写帧缓冲即可稳定显示；绝不能再手动写 CTL_START（会让 DSC 数据流失步、花屏）。

## 生成 payload（需要 macOS 构建出的内核）
    python3 tools/mkadt.py shim/out/venus.adt --highmem
    python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/venus.adt shim/out/payload.bin \
        --cmdline '<下面两个变体之一>'
    # 打包成 Android boot image header v3：kernel = shim 的 Image，ramdisk = payload.bin
    # （不要用 shim/build.sh 的 header v2 + DTB 方式，venus 的 ABL 会拒绝）
    fastboot boot venus-ios.img     # 只用 fb-guard 包装，绝不 flash

### 变体一：完整 iOS 启动（写 iOS 根文件系统）
前提：`userdata` 已刷入 `tools/mkiosroot.sh` 生成的 iOS 根镜像（`fastboot -S 512M flash userdata ios16-root.simg`，约 5–7 分钟，会清除 Android 用户数据）。

    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweettouch=1 sweetkeys=0 sweetusb=2'

### 变体二：只读测试（不挂载 iOS 根，不写 userdata）
与变体一相同，但 `ufsrw=0 rootrw=0`，并加上 `sweetlog=0 ufsroot=persist`。根文件系统挂载会按设计失败；USB、触摸等内核线程照常运行。

    --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=0 rootrw=0 launchd_unsecure_cache=1 wdt=-1 sweetfb_noconsole=1 sweetfb_hz=30 sweettouch=1 sweetkeys=0 sweetusb=2 sweetlog=0 ufsroot=persist'

## Custom boot logo (optional)
The shim can draw an image while it hands over to XNU.

    SHIM_LOGO_IMAGE=/path/to/logo.png shim/build.sh

- `SHIM_LOGO_MAX` (optional): limit for the longer side in pixels, default 400. The logo is drawn at 3x, centred, from row 1000.
- Requires Python 3 with Pillow.
- The image and the generated header stay local: `shim/out` is git-ignored. No logo is shipped with the repo.
- Tip: flat-colour logos compress far better than photos.

## 与 sweet 不同的值
| 项目 | sweet | venus |
| --- | --- | --- |
| 面板 | 1080x2400 | 1440x3200（K2，DSC 命令模式） |
| 帧缓冲 | 0x9c000000 / 0x1700000 | 0xe2a00000 / 0x2300000（实测 VIG0 扫描地址；32MB 块基址 0xe2000000，块内偏移 0xa00000；基础 DTB 写的 0xea600000 不对） |
| 虚拟 / 物理定时器 | INTID 19 / 18 | INTID 27 / 30 |
| IMEM restart_reason | 0x146aa65c | 0x146bf65c |
| 看门狗 | - | 0x17c10000：shim 连读都会卡死总线；它没有被 ABL 武装，所以 V60 式关闭（VENUS_WDT_DISABLE）默认关、从不访问 |
| CPU | A55x6 + A76x2 | A55x4 + A78x3 + X1x1，MPIDR 0x000-0x700 |
| 在线 CPU 数 | 未记录 | 8（cpus=8，PSCI CPU_ON，单一 SMP 调度域） |
| 触摸 | 未记录 | ST FTS（FTM5），QUP1 SE3 SPI 0x00a8c000（CS0，mode 0，4.8 MHz）；复位 GPIO22，中断 GPIO23（低有效），AVDD GPIO132；SPI 引脚 GPIO48-51（qup11）；VDD 为 PM8350C L2 1.8 V（RPMh "ldoc2"）；芯片 ID 36 39；坐标 0..1439 x 0..3199 |
| USB | 未记录 | DWC_usb31 v1.10a @0x0a600000（GSNPSID 0x33313130），QSCRATCH 0x0a6f8800，GDSC 0x10f004；HS femto PHY 0x088e3000（从未访问） |

相同的值：GICD 0x17a00000、GICR 0x17a60000（8 个 frame）、UFS 0x1d84000、SPMI arbiter 0x0c440000、--highmem 内存 0x1_00000000 + 0xfcc00000。

- RPMh apps RSC DRV 2 在 0x18220000，TCS 偏移 0xd00，布局与 sweet 相同。
- PM8350C 的 LDO 只能经 RPMh 控制，在 SPMI 上看不到。

## venus 补丁（0082–0086）
- 0082 venus-no-physical-timer：跳过 CNTP_CTL_EL0 写入（SM8350 上 EL1 物理定时器被 trap）。
- 0083 venus-usb：USB 探测使用 venus 的 GCC 时钟偏移（gcc-lahaina），不读 QUSB2 PHY（0x088e2000 不存在），DWC_usb31 v1.10a，gadget 保持高速（USB 2.0）。
- 0084 venus-usb-reset：gadget 像 Linux dwc3 一样处理总线复位和断开（结束传输、取消配置、重新挂起 ep0 SETUP），因为 Windows 主机（usbipd）会循环切换端口。
- 0085 venus-fts-touch：sweet_touch.c 重写为 ST FTS（FTM5）触摸芯片，经 GENI SPI 通信；sweet_rpmh.c 也接受 "ldoc2" 1.8 V。
- 0086 venus-touch-panel-size：SweetHIDTouch 按 1440x3200 限幅和边缘判断（原为 1080x2400）。

## shim 实验（EXPERIMENT=10/11/12，只读探测）
- 10 = U1：USB（GCC / DWC3 / QSCRATCH）。
- 11 = T1：触摸（TLMM GPIO 22/23/132，QUP1 时钟，SE3）。
- 12 = T2：QUP1 时钟投票 + SE3 固件版本 + 经 SPMI 读取 PMIC L2。

## 未验证
- PSHOLD 0x0c264000：venus DT 里没有，沿用 sweet。
- sweetlog 环在 iOS 根文件系统上不写入（原因不明）。
