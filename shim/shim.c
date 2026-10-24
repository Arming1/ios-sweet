/*
 * Milestone 2: one boot answers the questions that decide the XNU port.
 *   - CPU feature registers (16K granule, GIC sysreg interface)
 *   - PSCI/SMCCC version and which PSCI calls the Qualcomm TZ offers
 *   - SMP: PSCI CPU_ON for the 7 secondaries, each reports its MPIDR
 *   - GICv3 Group 1 IRQ from the virtual timer (INTID 19 on Qualcomm)
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "loader.h"
#include "ufs.h"

#define IMEM_RESTART_REASON	0x146bf65cUL	/* venus DT: qcom,msm-imem@146bf000 + restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL	/* venus: not in the DT, unverified */
#define WDT_BASE		0x17c10000UL	/* venus DT qcom,wdt@17c10000; same as V60, EN is +0x08 */

#define GICD_BASE		0x17a00000UL
#define GICR_BASE		0x17a60000UL
#define GICR_SIZE		0x100000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		27	/* DT: <GIC_PPI 3 ...>, not the usual 27 */
#define PTIMER_INTID		30	/* DT: <GIC_PPI 2 ...> */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static volatile uint64_t sec_mpidr[NR_CPUS];
static volatile uint64_t sec_probe[4];	/* CPU1 redistributor write test */
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", after_s > 20 ? "in 40s" : "in 20s");
	delay_ms(after_s * 1000);
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/*
 * venus heartbeat builds (-DHEARTBEAT=N): reboot straight to fastboot at
 * checkpoint N, with no delay and no console output. The phone returning
 * to fastboot proves the shim got that far (no screen or USB needed).
 */
#ifndef HEARTBEAT
#define HEARTBEAT 0
#endif
static void __attribute__((noreturn)) hb_reset(void)
{
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}
#define HB(n) do { if (HEARTBEAT == (n)) hb_reset(); } while (0)

/*
 * Timed heartbeat: report a 4-bit value with no screen or USB by waiting
 * 2 + 2*n seconds before the reboot. The host measures boot -> Android
 * (tools/run-heartbeat.sh) and subtracts the calibrated baseline.
 */
static void __attribute__((noreturn)) hb_report(unsigned n)
{
	delay_ms((2 + 2 * (n & 0xf)) * 1000);
	hb_reset();
}

/*
 * venus display experiments (-DEXPERIMENT=N), see notes/display-analysis.md.
 * 100+k: timing calibration, reports the constant k.
 * 1 (D1): read MDSS HW_VERSION at 0xae00000 (SDE 7.0 on SM8350 -> 0x7xxxxxxx)
 *         and report its top nibble. A hang means the MDP is unclocked.
 * 10 (U1): read-only USB probe (GCC power/clocks, DWC3, QSCRATCH), shown on screen.
 *          No register writes, never resets; the last line on screen names a hung read.
 */
#ifndef EXPERIMENT
#define EXPERIMENT 0
#endif
#define MDSS_BASE		0x0ae00000UL
/* Offsets from the venus DT (qcom,sde-*-off) and techpack/display/msm/sde/sde_hw_*.c */
static const uint32_t sspp_off[8] = {	/* VIG0..3, DMA0..3 */
	0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000,
};
#define SSPP_SRC_SIZE		0x00	/* (h << 16) | w */
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24	/* plane 0 stride in the low 16 bits */
#define CTL_OFF(i)		(0x16000 + 0x1000 * (i))	/* CTL_0..5 */
#define CTL_START		0x01c
#define CTL_INTF_ACTIVE		0x0f4
#define INTF1_BASE		(MDSS_BASE + 0x36000)	/* INTF_1 = DSI0, TE lives here on SDE 7 */
#define INTF_TIMING_ENGINE_EN	0x000
#define INTF_TEAR_CHECK_EN	0x284
#define INTF_AUTOREFRESH	0x2b4	/* BIT31 enable, low 16 bits frame count */
#define VENUS_SPLASH		0xea600000UL

static inline uint32_t mdp_rd(uint32_t off) { return rd32(MDSS_BASE + off); }

/* First SSPP whose SRC_SIZE is non-zero (the pipe ABL left fetching), or -1. */
static int active_pipe(void)
{
	for (int i = 0; i < 8; i++)
		if (mdp_rd(sspp_off[i] + SSPP_SRC_SIZE))
			return i;
	return -1;
}

/* CTL whose INTF_ACTIVE has INTF_1 (bit 1) set, or -1. */
static int intf1_ctl(void)
{
	for (int i = 0; i < 6; i++)
		if (mdp_rd(CTL_OFF(i) + CTL_INTF_ACTIVE) & (1u << 1))
			return i;
	return -1;
}

extern char _head[], _end[];	/* our own image (start.S / shim.ld) */
/*
 * D4 fill colour: pure blue as 32-bit 0xAARRGGBB. Seeing blue means the pipe
 * fetches ARGB; seeing red means R and B are swapped (ABGR). Either way the
 * panel refreshed.
 */
#define PAINT_COLOR		0xff0000ffU

/*
 * Fill a scanout buffer with PAINT_COLOR if it looks like ordinary RAM, is not huge
 * and does not overlap the shim itself (ABL chose our load address).
 */
static int paint(uint64_t pa, uint32_t stride, uint32_t rows)
{
	uint64_t len = (uint64_t)stride * rows;
	uint64_t lo = (uintptr_t)_head, hi = (uintptr_t)_end + 0x10000;	/* + stack slack */
	if (pa < 0x80000000UL || pa + len > 0x280000000UL || len == 0 || len > 0x4000000)
		return 0;
	if (pa < hi && lo < pa + len)
		return 0;
	for (uint64_t o = 0; o < len; o += 4)
		wr32(pa + o, PAINT_COLOR);
	__asm__ volatile("dsb sy" ::: "memory");
	return 1;
}

/* Paint every SSPP that ABL left fetching; returns how many buffers were painted. */
static unsigned paint_active_pipes(void)
{
	unsigned painted = 0;
	for (int i = 0; i < 8; i++) {
		uint32_t size = mdp_rd(sspp_off[i] + SSPP_SRC_SIZE);
		if (!size)
			continue;
		painted += paint(mdp_rd(sspp_off[i] + SSPP_SRC0_ADDR),
				 mdp_rd(sspp_off[i] + SSPP_SRC_YSTRIDE0) & 0xffff, size >> 16);
	}
	return painted;
}

/*
 * U1 (-DEXPERIMENT=10): read-only USB probe. No writes at all. Each register
 * pair is named on screen before it is read, so a hang leaves that line last.
 */
#define GCC_BASE		0x00100000UL
#define DWC3_BASE		0x0a600000UL
#define QSCRATCH_BASE		0x0a6f8800UL

struct u1_reg {	/* no pointers: the shim runs unrelocated, tables must hold no addresses of data */
	char name[8];
	uint32_t addr;
};

static const struct u1_reg u1_gcc[] = {	/* GDSCR bit31 = power on; CBCRs bit31 = CLK_OFF */
	{ "GDSCR", GCC_BASE + 0xf004 },	/* gcc-lahaina / venus DT qcom,gdsc@10f004 */
	{ "MASTER", GCC_BASE + 0xf010 },
	{ "SLEEP", GCC_BASE + 0xf018 },
	{ "MOCK", GCC_BASE + 0xf01c },
	{ "PHYAUX", GCC_BASE + 0xf054 },
	{ "PHYCOM", GCC_BASE + 0xf058 },
	{ "PHYPIPE", GCC_BASE + 0xf05c },
	{ "CFGNOC", GCC_BASE + 0xf07c },
	{ "AGGRE", GCC_BASE + 0xf080 },
};

static const struct u1_reg u1_dwc3[] = {
	{ "SNPSID", DWC3_BASE + 0xc120 },
	{ "GCTL", DWC3_BASE + 0xc110 },
	{ "GSTS", DWC3_BASE + 0xc118 },
	{ "USB2CFG", DWC3_BASE + 0xc200 },
	{ "HWPAR0", DWC3_BASE + 0xc140 },
	{ "DCFG", DWC3_BASE + 0xc700 },
	{ "DCTL", DWC3_BASE + 0xc704 },
	{ "DEVTEN", DWC3_BASE + 0xc708 },
	{ "DSTS", DWC3_BASE + 0xc70c },
	{ "DALEPEN", DWC3_BASE + 0xc720 },
	{ "EVADLO", DWC3_BASE + 0xc400 },
	{ "EVADHI", DWC3_BASE + 0xc404 },
	{ "EVSIZ", DWC3_BASE + 0xc408 },
	{ "EVCNT", DWC3_BASE + 0xc40c },
};

static const struct u1_reg u1_qscr[] = {
	{ "GENCFG", QSCRATCH_BASE + 0x08 },
	{ "HSPHY", QSCRATCH_BASE + 0x10 },
	{ "SSPHY", QSCRATCH_BASE + 0x30 },
};

/* Read n registers two per line; tag is the group name shown before each pair. */
static void u1_group(const char *tag, const struct u1_reg *r, uint32_t n, uint32_t *v)
{
	for (uint32_t i = 0; i < n; i += 2) {
		uint32_t two = i + 1 < n;
		con_hex2(tag, "next ", r[i].addr, two ? "next " : 0, two ? r[i + 1].addr : 0);
		v[i] = rd32(r[i].addr);
		if (two)
			v[i + 1] = rd32(r[i + 1].addr);
		__asm__ volatile("dsb sy" ::: "memory");
		con_hex2(r[i].name, "", v[i], two ? r[i + 1].name : 0, two ? v[i + 1] : 0);
	}
}

static void __attribute__((noreturn)) usb_u1_probe(void)
{
	uint32_t gv[sizeof(u1_gcc) / sizeof(u1_gcc[0])];
	uint32_t dv[sizeof(u1_dwc3) / sizeof(u1_dwc3[0])];
	uint32_t qv[sizeof(u1_qscr) / sizeof(u1_qscr[0])];

	con_clear(DARKGREY);
	con_str("USB", "u1 read-only probe, no writes");
	u1_group("GCC", u1_gcc, sizeof(u1_gcc) / sizeof(u1_gcc[0]), gv);
	/* DWC3 is only safe to touch with GCC power on and the master and cfg_noc clocks running */
	if ((gv[0] >> 31) && !(gv[1] >> 31) && !(gv[7] >> 31)) {
		con_str("USB", "next: DWC3");
		u1_group("DWC3", u1_dwc3, sizeof(u1_dwc3) / sizeof(u1_dwc3[0]), dv);
		/* QSCRATCH is the wrapper inside the same block: same power and clocks */
		con_str("USB", "next: QSCRATCH");
		u1_group("QSCR", u1_qscr, sizeof(u1_qscr) / sizeof(u1_qscr[0]), qv);
	} else {
		con_str("USB", "gated: DWC3/QSCRATCH not read");
	}
	con_str("USB", "done. hold Power 15s, then Power+VolDown");
	for (;;)
		__asm__ volatile("wfe");
}

static void run_experiment(void)
{
	if (EXPERIMENT >= 100 && EXPERIMENT < 116)
		hb_report(EXPERIMENT - 100);
	if (EXPERIMENT == 10)
		usb_u1_probe();
	if (EXPERIMENT == 1) {
		uint32_t v = rd32(MDSS_BASE);
		__asm__ volatile("dsb sy" ::: "memory");
		hb_report(v >> 28);
	}
	if (EXPERIMENT == 2) {	/* D2: which pipe scans the DT splash buffer? */
		for (int i = 0; i < 8; i++)
			if (mdp_rd(sspp_off[i] + SSPP_SRC0_ADDR) == (uint32_t)VENUS_SPLASH)
				hb_report(i);		/* 0..7: pipe i scans 0xea600000 */
		int a = active_pipe();
		hb_report(a >= 0 && a < 7 ? 8 + a : 15);	/* 8..14: pipe a active elsewhere; 15: none (or only DMA3) */
	}
	if (EXPERIMENT == 3) {	/* D3: INTF_1 state */
		uint32_t n = (mdp_rd(0x36000 + INTF_AUTOREFRESH) >> 31) & 1;
		n |= (mdp_rd(0x36000 + INTF_TEAR_CHECK_EN) & 1) << 1;
		n |= (mdp_rd(0x36000 + INTF_TIMING_ENGINE_EN) & 1) << 2;
		n |= (intf1_ctl() >= 0) << 3;
		hb_report(n);	/* bit0 autorefresh, bit1 tear check, bit2 timing engine, bit3 a CTL drives INTF_1 */
	}
	if (EXPERIMENT == 5) {	/* D3b: which CTL drives INTF_1 */
		int c = intf1_ctl();
		hb_report(c >= 0 ? c : 15);
	}
	if (EXPERIMENT >= 20 && EXPERIMENT < 28) {	/* address readout: nibble k of the active pipe's SRC0_ADDR */
		int a = active_pipe();
		if (a < 0)
			hb_report(15);
		hb_report(mdp_rd(sspp_off[a] + SSPP_SRC0_ADDR) >> (4 * ((EXPERIMENT - 20) & 7)));
	}
	if (EXPERIMENT == 4) {	/* D4: paint (blue) every active pipe's buffer + the DT splash, then kick INTF_1 */
		unsigned painted = paint_active_pipes();
		paint(VENUS_SPLASH, FB_W * 4, FB_H);
		int c = intf1_ctl();
		if (c >= 0) {
			wr32(INTF1_BASE + INTF_AUTOREFRESH, (1u << 31) | 1);
			__asm__ volatile("dsb sy" ::: "memory");
		}
		/* Hold ~30 s for a human to look at the panel; re-kick once a second. */
		for (int t = 0; t < 30; t++) {
			if (c >= 0)
				wr32(MDSS_BASE + CTL_OFF(c) + CTL_START, 1);
			delay_ms(1000);
		}
		hb_report((painted & 7) | ((c >= 0) << 3));	/* bits 0-2: buffers painted, bit 3: CTL found */
	}
	/*
	 * D4 showed blue for ~2 s, then DSC noise: ABL already leaves INTF_1
	 * autorefresh on, and our CTL_START kicks raced it. D5 only paints and
	 * touches no display register; D6 also rewrites autorefresh (same value
	 * as D4) but never kicks CTL_START, to tell the two writes apart.
	 */
	if (EXPERIMENT == 8 || EXPERIMENT == 9) {	/* D7 = 8, D8 = 9: the two pipes of the src-split */
		int p0 = -1, p1 = -1;
		for (int i = 0; i < 8; i++)
			if (mdp_rd(sspp_off[i] + SSPP_SRC_SIZE)) {
				if (p0 < 0)
					p0 = i;
				else if (p1 < 0)
					p1 = i;
			}
		if (EXPERIMENT == 8)
			hb_report(p1 >= 0 ? p1 : 15);	/* index of the second active pipe, 15 = none */
		if (p0 < 0 || p1 < 0)
			hb_report(15);
		uint32_t a0 = mdp_rd(sspp_off[p0] + SSPP_SRC0_ADDR), a1 = mdp_rd(sspp_off[p1] + SSPP_SRC0_ADDR);
		uint32_t n = (a1 == a0);				/* bit0: same buffer address */
		n |= (a1 == a0 + (FB_W / 2) * 4) << 1;			/* bit1: second pipe starts at the right half */
		n |= ((mdp_rd(sspp_off[p0] + SSPP_SRC_YSTRIDE0) & 0xffff) == FB_W * 4) << 2;	/* bit2: stride 5760 */
		n |= ((mdp_rd(sspp_off[p0] + SSPP_SRC_SIZE) & 0xffff) == FB_W / 2) << 3;	/* bit3: pipe 0 is 720 wide */
		hb_report(n);
	}
	if (EXPERIMENT == 6 || EXPERIMENT == 7) {	/* D5 = 6, D6 = 7 */
		unsigned painted = paint_active_pipes();
		uint32_t ar = mdp_rd(0x36000 + INTF_AUTOREFRESH);
		if (EXPERIMENT == 7) {
			wr32(INTF1_BASE + INTF_AUTOREFRESH, (1u << 31) | 1);
			__asm__ volatile("dsb sy" ::: "memory");
		}
		delay_ms(30000);	/* hold for a human to watch / film the panel */
		/* bits 0-2: buffers painted; bit 3: ABL's autorefresh frame count was 1 */
		hb_report((painted & 7) | (((ar & 0xffff) == 1) << 3));
	}
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

void secondary_main(uint64_t idx)
{
	sec_mpidr[idx] = rsr("mpidr_el1") | (1ULL << 63);
	if (idx == 1) {
		uintptr_t r = find_gicr();
		if (r) {
			sec_probe[0] = rd32(r + GICR_IGROUPR0);
			wr32(r + GICR_IGROUPR0, 0xffffffff);
			sec_probe[1] = rd32(r + GICR_IGROUPR0);
			wr8(r + GICR_IPRIORITYR + 19, 0xa0);
			sec_probe[2] = rd32(r + GICR_IPRIORITYR + 16);
		}
		sec_probe[3] = r ? 1 : 0xdead;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void __attribute__((unused)) test_smp(void)
{
	stage = "smp";
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	int64_t ret[NR_CPUS];

	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (((uint64_t)i << 8) == self)
			con_str(label, "is boot cpu");
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "mpidr=",
				 (uint32_t)sec_mpidr[i] | (sec_mpidr[i] ? 0 : 0xdead0000));
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t to_us(uint64_t ticks)
{
	return ticks * 10 / 192;	/* 19.2 MHz */
}

void shim_main(uint64_t dtb)
{
	struct mmu_info mi;

	HB(2);	/* entered shim_main */

	/* HB 7 probe: is the apps WDT enable register (WDT_BASE + 0x08) even readable? */
	if (HEARTBEAT == 7) {
		volatile uint32_t v = rd32(WDT_BASE + 0x08);
		(void)v;
		__asm__ volatile("dsb sy" ::: "memory");
		hb_reset();
	}

	/*
	 * venus: the apps watchdog disable (copied from the V60 port) HUNG the
	 * shim on this phone (heartbeat hb3, 2026-10-24). Off by default;
	 * enable only for experiments.
	 */
#if defined(VENUS_WDT_DISABLE) && VENUS_WDT_DISABLE
	wr32(WDT_BASE + 0x08, 0);
	__asm__ volatile("dsb sy" ::: "memory");
#endif
	HB(3);	/* shim_main past the (disabled) watchdog block */
	if (EXPERIMENT)
		run_experiment();	/* never returns for a defined experiment */

	/* An xnu payload in the ramdisk means: boot it (MMU stays off). */
	stage = "loader";
	con_clear(DARKGREY);
	HB(4);	/* framebuffer written */
	con_str("SHIM", "sweet milestone 4: xnu loader");
	con_skip();
	const char *why = loader_boot(dtb);
	con_str("LOADER", why);
	con_skip();
	HB(5);	/* loader returned (no payload) */

	/* No payload: probe the internal UFS while the MMU is still off. */
	stage = "ufs";
	test_ufs();
	HB(6);	/* UFS probe returned */
	con_skip();
	/* Keep the probe results on screen (later tests clear it); reset after 3 min. */
	con_str("UFS", "probe done, reset in ~3 min");
	delay_ms(180000);
	reset(20);

	stage = "fill off";
	uint64_t fill_off = ticks_fill(DARKGREY);
	uint64_t mem_off = ticks_mem();

	stage = "mmu enable";
	int rc = mmu_enable(&mi);

	stage = "fill on";
	uint64_t fill_on = rc ? 0 : ticks_fill(DARKGREY);
	uint64_t mem_on = rc ? 0 : ticks_mem();
	if (rc)
		con_clear(DARKGREY);

	con_str("SHIM", "sweet milestone 3");
	con_skip();
	con_hex2("DTB", "at=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	con_hex2("IMAGE", "lo=", mi.img_lo, "hi=", mi.img_hi);
	con_hex2("MMU", "rc=", rc, "l3=", mi.n_l3);
	con_hex2("TTBR0", "pa=", mi.ttbr, "pa_hi=", mi.ttbr >> 32);
	con_hex("SCTLR0", mi.sctlr_before);
	con_hex("SCTLR1", mi.sctlr_after);
	con_hex("TCR", rsr("tcr_el1"));
	con_hex2("PARANGE", "mmfr0=", rsr("id_aa64mmfr0_el1") & 0xf, "tg16=", (rsr("id_aa64mmfr0_el1") >> 20) & 0xf);
	con_skip();
	con_hex("PAR img", mmu_probe((uintptr_t)shim_main));
	con_hex("PAR fb", mmu_probe(FB_PHYS + 0x1000));
	con_hex("PAR gic", mmu_probe(GICD_BASE));
	con_hex("PAR 12G", mmu_probe(0x300000000UL));
	con_skip();
	con_hex2("FILL us", "off=", to_us(fill_off), "on=", to_us(fill_on));
	con_hex2("MEM us", "off=", to_us(mem_off), "on=", to_us(mem_on));
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_skip();
	test_gic();

	stage = "done";
	reset(40);
}
