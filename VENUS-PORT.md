# venus（Xiaomi Mi 11, SM8350）移植说明

`apply-venus-port.py` 在这个工作副本上做的改动，数值依据见上层项目的 `notes/patch-analysis.md`。

## 第一次启动目标
shim 颜色测试和 MMFR0 读数；之后 XNU 把控制台日志画到帧缓冲上。触摸、按键、USB、电池驱动是 sweet 专用的，全部关掉。

## 生成 payload（需要 macOS 构建出的内核）
    python3 tools/mkadt.py shim/out/venus.adt --highmem
    python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/venus.adt shim/out/payload.bin \
        --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweettouch=0 sweetkeys=0 sweetusb=0'
    DTB=/path/to/venus.dtb shim/build.sh ../shim/out/payload.bin
    fastboot boot shim/out/shim-boot.img     # 只用 fb-guard 包装，绝不 flash

## 与 sweet 不同的值
| 项目 | sweet | venus |
| --- | --- | --- |
| 面板 | 1080x2400 | 1440x3200（K2，DSC 命令模式） |
| 帧缓冲 | 0x9c000000 / 0x1700000 | 0xea600000 / 0x2300000（32MB 块基址 0xea000000，块内偏移 0x600000） |
| 虚拟 / 物理定时器 | INTID 19 / 18 | INTID 27 / 30 |
| IMEM restart_reason | 0x146aa65c | 0x146bf65c |
| 看门狗 | - | 0x17c10000，shim 里把 EN 写 0（按 V60 做法，未实测） |
| CPU | A55x6 + A76x2 | A55x4 + A78x3 + X1x1，MPIDR 0x000-0x700 |

相同的值：GICD 0x17a00000、GICR 0x17a60000、UFS 0x1d84000、--highmem 内存 0x1_00000000 + 0xfcc00000。

## 未验证
- PSHOLD 0x0c264000：venus DT 里没有，沿用 sweet。
- ABL 是否真的把画面放在 0xea600000、命令模式面板是否只写内存就会刷新。
- A55 小核是否支持 16K 页（shim 会打印 tg16）。
