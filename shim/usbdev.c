/*
 * U2 (-DEXPERIMENT=16): USB 2.0 high-speed vendor-class device on venus.
 *
 * Port of the working XNU gadget (bsd/dev/sweet_usb.c, patches 0072-0089,
 * reconstructed from the patch series): DWC_usb31 v1.10a at 0x0a600000 in
 * device mode, the ep0 SETUP/DATA/STATUS state machine with TRBs, bus reset
 * and disconnect handling (patch 0084), connect-done endpoint 0 setup, SET_
 * ADDRESS, SET_CONFIGURATION enabling the bulk endpoints, and the QSCRATCH VBUS
 * overrides. Network, RNDIS and the interrupt endpoint are left out.
 *
 * Differences from XNU: MMIO is physical (identity map); there are no locks,
 * threads or interrupts, usbdev_poll() consumes the event buffer; delays use
 * the generic timer; nothing prints. Descriptors add a BOS with the MS OS 2.0
 * platform capability so Windows asks for the WinUSB compatible ID.
 *
 * DMA area (usb_dma, 16 KiB, mapped Normal non-cacheable by mmu_set_nc_window;
 * physical address == virtual address):
 *   0x00000  event buffer, 4 KiB (GEVNTSIZ)
 *   0x01000  TRBs: ep0 OUT 0x1000, ep0 IN 0x1010, bulk OUT buffer 0 0x1020,
 *            buffer 1 0x1030 (16 bytes each)
 *   0x01040  ep0 SETUP packet, 8 bytes
 *   0x01100  ep0 data stage, 512 bytes (descriptors are at most 162 bytes)
 *   other    unused
 *
 * Bulk OUT buffers (usb_rx, 2 x 32 KiB) are not in the DMA area: they are
 * ordinary cacheable memory (Normal WB), so the parser reads them at cache
 * speed. The cache is maintained around each transfer: clean + invalidate
 * before the buffer is armed, invalidate the received bytes before the
 * callback reads them (dcache_*_range, mmu.c).
 */
#include <stdint.h>
#include <stdbool.h>
#include "mmu.h"
#include "usbdev.h"

/* ---- venus platform ---- */
#define GCC_BASE		0x00100000UL
#define GCC_USB30_PRIM_GDSCR	0x0f004		/* bit 31: power on */
#define GCC_MASTER_CBCR		0x0f010		/* bit 31: CLK_OFF */
#define GCC_CFG_NOC_CBCR	0x0f07c		/* bit 31: CLK_OFF */
#define CBCR_CLK_OFF		(1u << 31)

#define DWC3_BASE		0x0a600000UL
#define GSNPSID_DWC_USB31	0x33313130U	/* DWC_usb31 v1.10a */

#define QSCRATCH_BASE		0x0a6f8800UL
#define QSCRATCH_HS_PHY_CTRL	0x10
#define QSCRATCH_SS_PHY_CTRL	0x30
#define UTMI_OTG_VBUS_VALID	(1u << 20)
#define SW_SESSVLD_SEL		(1u << 28)
#define LANE0_PWR_PRESENT	(1u << 24)

/* ---- DWC3 device registers ---- */
#define GSNPSID		0xc120
#define GEVNTADRLO	0xc400
#define GEVNTADRHI	0xc404
#define GEVNTSIZ	0xc408
#define GEVNTCOUNT	0xc40c
#define DCFG		0xc700
#define DCTL		0xc704
#define DEVTEN		0xc708
#define DSTS		0xc70c
#define DALEPENA	0xc720
#define DEPCMDPAR2(n)	(0xc800 + 0x10 * (n))
#define DEPCMDPAR1(n)	(0xc804 + 0x10 * (n))
#define DEPCMDPAR0(n)	(0xc808 + 0x10 * (n))
#define DEPCMD(n)	(0xc80c + 0x10 * (n))

#define DCTL_RUN_STOP		(1u << 31)
#define DCTL_CSFTRST		(1u << 30)
#define DSTS_DEVCTRLHLT		(1u << 22)
#define DSTS_SPEED_MASK		7u
#define DCFG_SPEED_MASK		7u
#define DCFG_ADDR_MASK		(0x7fu << 3)

#define DEPCMD_SETEPCONFIG	1
#define DEPCMD_SETTRANSFRESOURCE 2
#define DEPCMD_SETSTALL		4
#define DEPCMD_STARTTRANSFER	6
#define DEPCMD_ENDTRANSFER	8
#define DEPCMD_DEPSTARTCFG	9
#define DEPCMD_CMDACT		(1u << 10)
#define DEPCMD_HIPRI_FORCERM	(1u << 11)
#define DEPCMD_PARAM(x)		((uint32_t)(x) << 16)

#define TRB_HWO			(1u << 0)
#define TRB_LST			(1u << 1)
#define TRB_ISP_IMI		(1u << 10)
#define TRB_IOC			(1u << 11)
#define TRBCTL_NORMAL		(1u << 4)
#define TRBCTL_SETUP		(2u << 4)
#define TRBCTL_STATUS2		(3u << 4)	/* control status, no data stage */
#define TRBCTL_STATUS3		(4u << 4)	/* control status after a data stage */
#define TRBCTL_DATA		(5u << 4)

/* Physical endpoints: 2n = OUT, 2n+1 = IN. */
#define EP_RX		2		/* 0x01 bulk OUT (host -> phone) */
#define EP_TX		3		/* 0x81 bulk IN (declared, unused) */

#define MSOS_VENDOR_CODE	0x20

/* ---- DMA area layout (offsets) ---- */
#define DMA_SIZE	16384
#define DMA_EVT		0x00000
#define EVT_SIZE	0x1000
#define TRB_EP0_OUT	0x01000
#define TRB_EP0_IN	0x01010
#define TRB_RX0		0x01020
#define TRB_RX1		0x01030
#define DMA_SETUP	0x01040
#define DMA_EP0_DATA	0x01100		/* 512 bytes */
#define RX_SIZE		0x8000		/* one bulk OUT buffer, 32 KiB */
#define CACHE_ALIGN	4096		/* bulk OUT buffers: page aligned (a multiple of the 64-byte line) */

/* usbdev_state_bits */
#define SB_RESET	(1u << 0)
#define SB_CONNECT	(1u << 1)
#define SB_HS		(1u << 2)
#define SB_ADDR		(1u << 3)
#define SB_CONF		(1u << 4)

/* ---- descriptors ---- */

/* Device: USB 2.1 (so the host asks for the BOS), per-interface class, VID 1209 PID 0003. */
static const uint8_t u2_dev_desc[18] = {
	18, 1, 0x10, 0x02, 0x00, 0x00, 0x00, 64,
	0x09, 0x12, 0x03, 0x00, 0x00, 0x01, 1, 2, 3, 1,
};

/* Configuration: 1 interface (vendor class), bulk OUT 0x01 and bulk IN 0x81, 512 bytes, 500 mA. */
static const uint8_t u2_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 250,		/* configuration: 1 interface */
	9, 4, 0, 0, 2, 0xff, 0x00, 0x00, 0,		/* interface 0: vendor, 2 endpoints */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,		/* EP 0x01 bulk OUT, 512 */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,		/* EP 0x81 bulk IN, 512 */
};

/* Device qualifier (high speed only device: other-speed config requests stall). */
static const uint8_t u2_qual_desc[10] = {
	10, 6, 0x10, 0x02, 0x00, 0x00, 0x00, 64, 1, 0,
};

/*
 * BOS: one platform capability, the MS OS 2.0 descriptor (UUID
 * D8DD60DF-4589-4CC7-9CD2-659D9E648A9F in little-endian field order),
 * dwWindowsVersion 0x06030000 (Windows 8.1), set length 162, vendor code 0x20.
 */
static const uint8_t u2_bos_desc[33] = {
	5, 0x0f, 33, 0, 1,				/* BOS: wTotalLength 5 + 28, 1 capability */
	28, 0x10, 0x05, 0,				/* platform capability, PlatformCapability */
	0xdf, 0x60, 0xdd, 0xd8, 0x89, 0x45, 0xc7, 0x4c,	/* PlatformCapabilityUUID */
	0x9c, 0xd2, 0x65, 0x9d, 0x9e, 0x64, 0x8a, 0x9f,
	0x00, 0x00, 0x03, 0x06,				/* dwWindowsVersion 0x06030000 */
	162, 0,						/* wMSOSDescriptorSetTotalLength */
	MSOS_VENDOR_CODE, 0,				/* bMS_VendorCode, bAltEnumCode */
};

/*
 * MS OS 2.0 descriptor set (returned for bRequest 0x20, wIndex 7):
 *   header (10): wLength, type 0, dwWindowsVersion, wTotalLength 162
 *   compatible ID (20): WINUSB, sub-compatible ID zero
 *   registry property (132): REG_MULTI_SZ "DeviceInterfaceGUIDs" (name 42
 *     bytes incl. NUL) = "{3B6C5B1E-5A0A-4C2E-9F4B-5357454554F1}" + double NUL
 *     (data 80 bytes). UTF-16LE throughout.
 */
static const uint8_t u2_msos_desc[162] = {
	0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0xa2, 0x00,
	0x14, 0x00, 0x03, 0x00, 'W', 'I', 'N', 'U', 'S', 'B',
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0x84, 0x00, 0x04, 0x00, 0x07, 0x00, 0x2a, 0x00,
	'D', 0, 'e', 0, 'v', 0, 'i', 0,
	'c', 0, 'e', 0, 'I', 0, 'n', 0,
	't', 0, 'e', 0, 'r', 0, 'f', 0,
	'a', 0, 'c', 0, 'e', 0, 'G', 0,
	'U', 0, 'I', 0, 'D', 0, 's', 0,
	0, 0,
	0x50, 0x00,
	'{', 0, '3', 0, 'B', 0, '6', 0,
	'C', 0, '5', 0, 'B', 0, '1', 0,
	'E', 0, '-', 0, '5', 0, 'A', 0,
	'0', 0, 'A', 0, '-', 0, '4', 0,
	'C', 0, '2', 0, 'E', 0, '-', 0,
	'9', 0, 'F', 0, '4', 0, 'B', 0,
	'-', 0, '5', 0, '3', 0, '5', 0,
	'7', 0, '4', 0, '5', 0, '4', 0,
	'5', 0, '5', 0, '4', 0, 'F', 0,
	'1', 0, '}', 0, 0, 0, 0, 0,
};

_Static_assert(sizeof(u2_dev_desc) == 18, "device descriptor length");
_Static_assert(sizeof(u2_cfg_desc) == 32, "configuration descriptor length (wTotalLength)");
_Static_assert(sizeof(u2_bos_desc) == 33, "BOS length (wTotalLength 5 + 28)");
_Static_assert(sizeof(u2_msos_desc) == 162, "MS OS 2.0 set length (wMSOSDescriptorSetTotalLength)");

/* ---- driver state ---- */

/* DMA area: NC mapping by the shim (usbdev_dma_window). 16 KiB aligned for whole pages. */
static uint8_t usb_dma[DMA_SIZE] __attribute__((aligned(16384)));
/* bulk OUT buffers: cacheable, maintained by ud_rx_arm / ud_rx_done */
static uint8_t usb_rx[2][RX_SIZE] __attribute__((aligned(CACHE_ALIGN)));

enum ep0_state { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS2, EP0_WAIT_STATUS3, EP0_STATUS };

static struct {
	uintptr_t dma;				/* usb_dma; VA == PA */
	bool touched;				/* registers changed: usbdev_stop restores ABL state */
	bool running;
	bool configured;
	bool connected, hs, addressed, reset_seen;
	enum ep0_state ep0;
	bool data_in;				/* ep0 data stage direction */
	bool xfer_active[6];
	uint8_t xfer_rsc[6];
	uint8_t rx_armed;			/* bulk OUT buffer (0/1) in the active transfer */
	uint32_t evt_pos;
	/* ABL state, restored by usbdev_stop */
	uint32_t abl_dcfg, abl_devten, abl_dalepena;
	uint32_t abl_gevntadrlo, abl_gevntadrhi, abl_gevntsiz;
	uint32_t abl_qs_hs, abl_qs_ss;
	void (*rx)(const uint8_t *, uint32_t);
	uint32_t events, setups, rx_xfers, epcmd_err;
} ud;

static inline void dwc_wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(DWC3_BASE + off) = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t dwc_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(DWC3_BASE + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline uint32_t mmio_rd(uintptr_t a)
{
	uint32_t v = *(volatile uint32_t *)a;
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void mmio_wr(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t ud_now(void)
{
	uint64_t v;
	__asm__ volatile("isb" ::: "memory");
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void ud_delay_us(uint32_t us)
{
	uint64_t f, end;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	end = ud_now() + f * us / 1000000;
	while (ud_now() < end)
		;
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++)
		dst[i] = src[i];
}

/* DEPCMD on physical endpoint ep; 0 on success. */
static int ud_epcmd(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	dwc_wr(DEPCMDPAR0(ep), p0);
	dwc_wr(DEPCMDPAR1(ep), p1);
	dwc_wr(DEPCMDPAR2(ep), p2);
	dwc_wr(DEPCMD(ep), cmd | DEPCMD_CMDACT);
	for (int i = 0; i < 1000; i++) {
		uint32_t v = dwc_rd(DEPCMD(ep));
		if (!(v & DEPCMD_CMDACT)) {
			if ((v >> 12) & 0xf) {
				ud.epcmd_err++;
				return -1;
			}
			return 0;
		}
		ud_delay_us(1);
	}
	ud.epcmd_err++;
	return -1;
}

/* One TRB at DMA offset trb: buffer at physical address bpa, len bytes, control bits ctl. */
static void ud_write_trb(uint32_t trb, uintptr_t bpa, uint32_t len, uint32_t ctl)
{
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + trb);

	t[0] = (uint32_t)bpa;
	t[1] = (uint32_t)(bpa >> 32);
	t[2] = len;
	t[3] = ctl | TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC;
	__asm__ volatile("dsb sy" ::: "memory");
}

/* StartTransfer on physical endpoint ep with the TRB at DMA offset trb. */
static void ud_start_trb(unsigned ep, uint32_t trb, uintptr_t bpa, uint32_t len, uint32_t ctl)
{
	uintptr_t tpa;

	ud_write_trb(trb, bpa, len, ctl);
	tpa = ud.dma + trb;
	if (ud_epcmd(ep, DEPCMD_STARTTRANSFER, (uint32_t)(tpa >> 32), (uint32_t)tpa, 0) == 0 && ep < 6) {
		ud.xfer_active[ep] = true;
		ud.xfer_rsc[ep] = (dwc_rd(DEPCMD(ep)) >> 16) & 0x7f;
	}
}

/* ep0: ep 0 = OUT, 1 = IN; buf is a DMA area offset. */
static void ud_ep0_start(unsigned ep, uint32_t buf, uint32_t len, uint32_t ctl)
{
	ud_start_trb(ep, ep ? TRB_EP0_IN : TRB_EP0_OUT, ud.dma + buf, len, ctl);
}

static void ud_start_setup(void)
{
	ud.ep0 = EP0_SETUP;
	ud_ep0_start(0, DMA_SETUP, 8, TRBCTL_SETUP);
}

static void ud_stall(void)
{
	ud_epcmd(0, DEPCMD_SETSTALL, 0, 0, 0);
	ud_start_setup();
}

/* Bus reset or disconnect (Linux dwc3 dwc3_stop_active_transfers). */
static void ud_end_transfer(unsigned ep)
{
	if (ud.xfer_active[ep]) {
		ud_epcmd(ep, DEPCMD_ENDTRANSFER | DEPCMD_HIPRI_FORCERM | DEPCMD_PARAM(ud.xfer_rsc[ep]), 0, 0, 0);
		ud_delay_us(100);
		ud.xfer_active[ep] = false;
	}
}

static void ud_stop_data(void)
{
	ud_end_transfer(EP_RX);
	ud_end_transfer(EP_TX);
	dwc_wr(DALEPENA, 0x3);
	ud.configured = false;
}

/*
 * Bulk OUT: arm buffer idx (0 or 1) as one 32 KiB NORMAL TRB, ISP so a short packet completes it.
 * Clean + invalidate the whole buffer first: a dirty line evicted during the DMA would overwrite the data.
 */
static void ud_rx_arm(void)
{
	uint32_t idx = ud.rx_armed;
	uint8_t *buf = usb_rx[idx];

	dcache_clean_inval_range((uintptr_t)buf, (uintptr_t)buf + RX_SIZE);
	ud_start_trb(EP_RX, idx ? TRB_RX1 : TRB_RX0, (uintptr_t)buf, RX_SIZE, TRBCTL_NORMAL);
}

/*
 * XferComplete on EP_RX: arm the other buffer first, then hand this one to the callback.
 * The received bytes are invalidated first, so the callback reads what the DMA wrote.
 */
static void ud_rx_done(void)
{
	uint32_t cur = ud.rx_armed;
	volatile uint32_t *t = (volatile uint32_t *)(ud.dma + (cur ? TRB_RX1 : TRB_RX0));
	uint32_t len = RX_SIZE - (t[2] & 0xffffff);
	uint8_t *buf = usb_rx[cur];

	ud.rx_armed = cur ^ 1;
	ud_rx_arm();
	ud.rx_xfers++;
	if (ud.rx != 0) {
		dcache_inval_range((uintptr_t)buf, (uintptr_t)buf + len);
		ud.rx(buf, len);
	}
}

/* SET_CONFIGURATION(1): bulk OUT and IN endpoints, then arm bulk OUT. */
static bool ud_configure(void)
{
	if (ud.configured)
		return true;
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG | DEPCMD_PARAM(2), 0, 0, 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3), (1u << 8) | ((uint32_t)EP_RX << 25), 0) ||
	    ud_epcmd(EP_RX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETEPCONFIG, (2u << 1) | (512u << 3) | (1u << 17), (1u << 8) | ((uint32_t)EP_TX << 25), 0) ||
	    ud_epcmd(EP_TX, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0))
		return false;
	dwc_wr(DALEPENA, dwc_rd(DALEPENA) | (1u << EP_RX) | (1u << EP_TX));
	ud.configured = true;
	ud.rx_armed = 0;
	ud_rx_arm();
	return true;
}

/*
 * ASCII text as a UTF-16LE string descriptor into out; returns its length.
 * Called with literals one by one: a switch of string pointers would make GCC
 * emit absolute addresses (R_AARCH64_ABS64), which build.sh rejects.
 */
static uint32_t ud_put_str(uint8_t *out, const char *s)
{
	uint32_t n = 2;

	for (; *s != 0; s++) {
		out[n++] = (uint8_t)*s;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

/* String descriptor idx into out; returns its length, 0 if there is no such string. */
static uint32_t ud_string(unsigned idx, uint8_t *out)
{
	if (idx == 0) {
		out[0] = 4;
		out[1] = 3;
		out[2] = 0x09;	/* en-US */
		out[3] = 0x04;
		return 4;
	}
	if (idx == 1)
		return ud_put_str(out, "Sweet");
	if (idx == 2)
		return ud_put_str(out, "Sweet shim video");
	if (idx == 3)
		return ud_put_str(out, "0001");
	return 0;
}

/* GET_DESCRIPTOR: value = type << 8 | index. Writes the descriptor to out, returns its length (0 = stall). */
static uint32_t ud_get_descriptor(uint16_t value, uint8_t *out)
{
	switch (value >> 8) {
	case 1:
		copy_bytes(out, u2_dev_desc, sizeof(u2_dev_desc));
		return sizeof(u2_dev_desc);
	case 2:
		if ((value & 0xff) != 0)
			return 0;
		copy_bytes(out, u2_cfg_desc, sizeof(u2_cfg_desc));
		return sizeof(u2_cfg_desc);
	case 3:
		return ud_string(value & 0xff, out);
	case 6:
		copy_bytes(out, u2_qual_desc, sizeof(u2_qual_desc));
		return sizeof(u2_qual_desc);
	case 0x0f:
		copy_bytes(out, u2_bos_desc, sizeof(u2_bos_desc));
		return sizeof(u2_bos_desc);
	default:
		return 0;
	}
}

/* SETUP packet: answer it, then start the data stage (IN) or wait for the status stage. */
static void ud_setup(void)
{
	const uint8_t *st = (const uint8_t *)(ud.dma + DMA_SETUP);
	uint8_t *data = (uint8_t *)(ud.dma + DMA_EP0_DATA);
	uint8_t type = st[0], req = st[1];
	uint16_t value = (uint16_t)(st[2] | st[3] << 8);
	uint16_t index = (uint16_t)(st[4] | st[5] << 8);
	uint16_t length = (uint16_t)(st[6] | st[7] << 8);
	uint32_t n = 0;
	bool ok = true;

	ud.setups++;
	ud.data_in = (type & 0x80) != 0;
	if (type == 0xc0 && req == MSOS_VENDOR_CODE && index == 7) {
		/* MS OS 2.0 descriptor set */
		n = sizeof(u2_msos_desc);
		copy_bytes(data, u2_msos_desc, n);
	} else if ((type & 0x60) != 0) {
		ok = false;				/* class and vendor requests: none here */
	} else if (type == 0x80 && req == 6) {		/* GET_DESCRIPTOR */
		n = ud_get_descriptor(value, data);
		ok = n != 0;
	} else if (type == 0x00 && req == 5) {		/* SET_ADDRESS: DWC3 applies it now */
		dwc_wr(DCFG, (dwc_rd(DCFG) & ~DCFG_ADDR_MASK) | ((uint32_t)(value & 0x7f) << 3));
		ud.addressed = (value & 0x7f) != 0;
	} else if (type == 0x00 && req == 9) {		/* SET_CONFIGURATION */
		if (value == 1) {
			ok = ud_configure();
		} else if (value == 0) {
			ud_stop_data();
		} else {
			ok = false;
		}
	} else if (type == 0x80 && req == 8) {		/* GET_CONFIGURATION */
		data[0] = ud.configured ? 1 : 0;
		n = 1;
	} else if ((type == 0x80 || type == 0x81 || type == 0x82) && req == 0) {	/* GET_STATUS: bus powered */
		data[0] = 0;
		data[1] = 0;
		n = 2;
	} else if ((type == 0x00 || type == 0x01 || type == 0x02) && (req == 1 || req == 3)) {
		/* CLEAR/SET_FEATURE (halt, remote wakeup): accepted, no effect */
	} else if (type == 0x01 && req == 11) {		/* SET_INTERFACE: alternate 0 of interface 0 only */
		ok = value == 0 && index == 0;
	} else if (type == 0x81 && req == 10) {	/* GET_INTERFACE */
		data[0] = 0;
		n = 1;
	} else {
		ok = false;
	}
	if (!ok || (!ud.data_in && length != 0)) {	/* host-to-device data stages: none here */
		ud_stall();
		return;
	}
	if (length == 0) {
		ud.ep0 = EP0_WAIT_STATUS2;		/* status stage starts on XferNotReady */
		return;
	}
	ud.ep0 = EP0_DATA;
	if (n > length)
		n = length;
	ud_ep0_start(1, DMA_EP0_DATA, n, TRBCTL_DATA);
}

/* ---- events (XNU sweet_usb.c ud_event, without the network) ---- */

static void ud_device_event(uint32_t type)
{
	if (type == 1) {			/* USB reset */
		ud.reset_seen = true;
		ud.connected = false;
		ud.hs = false;
		ud.addressed = false;
		ud_stop_data();
		/* ep0 mid transfer: end it, stall and re-arm SETUP (dwc3_ep0_stall_and_restart) */
		if (ud.ep0 != EP0_SETUP) {
			ud_end_transfer(0);
			ud_end_transfer(1);
			ud_stall();
		}
		dwc_wr(DCFG, dwc_rd(DCFG) & ~DCFG_ADDR_MASK);
	} else if (type == 2) {			/* connection done: ep0 max packet 64 (modify action) */
		ud.connected = true;
		ud.hs = (dwc_rd(DSTS) & DSTS_SPEED_MASK) == 0;
		ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			 (1u << 8) | (1u << 10) | (0u << 25), 0);
		ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (64u << 3) | (2u << 30),
			 (1u << 8) | (1u << 10) | (1u << 25), 0);
	} else if (type == 0) {			/* disconnect */
		ud.connected = false;
		ud.hs = false;
		ud_stop_data();
	}
}

static void ud_ep_event(uint32_t ev)
{
	unsigned ep = (ev >> 1) & 0x1f;
	unsigned type = (ev >> 6) & 0xf;	/* 1 XferComplete, 3 XferNotReady */
	unsigned status = (ev >> 12) & 0xf;	/* XferNotReady: 2 = control status phase */
	bool was_active = ep < 6 && ud.xfer_active[ep];

	if (ep < 6 && type == 1)
		ud.xfer_active[ep] = false;
	if (ep == EP_RX) {
		/* a completion for a transfer ended by a reset is ignored */
		if (type == 1 && was_active)
			ud_rx_done();
		return;
	}
	if (ep > 1)
		return;				/* the bulk IN endpoint has no transfers yet */
	if (type == 1) {			/* ep0 XferComplete */
		switch (ud.ep0) {
		case EP0_SETUP:
			ud_setup();
			break;
		case EP0_DATA:
			ud.ep0 = EP0_WAIT_STATUS3;
			break;
		case EP0_STATUS:
			ud_start_setup();
			break;
		default:
			break;
		}
		return;
	}
	if (type == 3 && status == 2) {		/* XferNotReady, control status phase */
		if (ud.ep0 == EP0_WAIT_STATUS2 || ud.ep0 == EP0_WAIT_STATUS3) {
			uint32_t ctl = ud.ep0 == EP0_WAIT_STATUS3 ? TRBCTL_STATUS3 : TRBCTL_STATUS2;
			ud.ep0 = EP0_STATUS;
			ud_ep0_start(ep, DMA_EP0_DATA, 0, ctl);
		}
	}
}

static void ud_event(uint32_t ev)
{
	if (ev & 1)
		ud_device_event((ev >> 8) & 0xf);
	else
		ud_ep_event(ev);
}

/* ---- public API ---- */

int usbdev_init(void (*rx)(const uint8_t *, uint32_t))
{
	uint32_t i;

	if (ud.touched)
		return -1;
	if (!(mmio_rd(GCC_BASE + GCC_USB30_PRIM_GDSCR) & (1u << 31)) ||
	    (mmio_rd(GCC_BASE + GCC_MASTER_CBCR) & CBCR_CLK_OFF) ||
	    (mmio_rd(GCC_BASE + GCC_CFG_NOC_CBCR) & CBCR_CLK_OFF))
		return -2;		/* GDSC off or master / config-NoC clock gated: DWC3 must not be touched */
	if (dwc_rd(GSNPSID) != GSNPSID_DWC_USB31)
		return -3;

	ud.dma = (uintptr_t)usb_dma;
	ud.rx = rx;
	ud.reset_seen = ud.connected = ud.hs = ud.addressed = false;
	ud.events = ud.setups = ud.rx_xfers = ud.epcmd_err = 0;
	ud.touched = true;
	ud.abl_dcfg = dwc_rd(DCFG);
	ud.abl_devten = dwc_rd(DEVTEN);
	ud.abl_dalepena = dwc_rd(DALEPENA);
	ud.abl_gevntadrlo = dwc_rd(GEVNTADRLO);
	ud.abl_gevntadrhi = dwc_rd(GEVNTADRHI);
	ud.abl_gevntsiz = dwc_rd(GEVNTSIZ);
	ud.abl_qs_hs = mmio_rd(QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL);
	ud.abl_qs_ss = mmio_rd(QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL);

	/* device controller soft reset (PHY and global setup stay as ABL left them) */
	dwc_wr(DCTL, DCTL_CSFTRST);
	for (i = 0; i < 500 && (dwc_rd(DCTL) & DCTL_CSFTRST); i++)
		ud_delay_us(1000);
	if (dwc_rd(DCTL) & DCTL_CSFTRST)
		return -4;

	dwc_wr(GEVNTADRLO, (uint32_t)(ud.dma + DMA_EVT));
	dwc_wr(GEVNTADRHI, (uint32_t)((ud.dma + DMA_EVT) >> 32));
	dwc_wr(GEVNTSIZ, (1u << 31) | EVT_SIZE);	/* interrupt masked: polled */
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);
	ud.evt_pos = 0;

	/* high speed (DEVSPD 0; ABL ran fastboot at SuperSpeed), address 0 */
	dwc_wr(DCFG, dwc_rd(DCFG) & ~(DCFG_SPEED_MASK | DCFG_ADDR_MASK));
	dwc_wr(DEVTEN, 0x7);		/* disconnect, USB reset, connect done */

	/* ep0: start config, control type, max packet 512 until connect done */
	if (ud_epcmd(0, DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
	    ud_epcmd(0, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (0u << 25), 0) ||
	    ud_epcmd(0, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0) ||
	    ud_epcmd(1, DEPCMD_SETEPCONFIG, (0u << 1) | (512u << 3), (1u << 8) | (1u << 10) | (1u << 25), 0) ||
	    ud_epcmd(1, DEPCMD_SETTRANSFRESOURCE, 1, 0, 0))
		return -6;
	dwc_wr(DALEPENA, 0x3);
	ud_start_setup();

	/* VBUS and session valid forced in software (Linux dwc3-qcom vbus override) */
	mmio_wr(QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL, ud.abl_qs_hs | UTMI_OTG_VBUS_VALID | SW_SESSVLD_SEL);
	mmio_wr(QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL, ud.abl_qs_ss | LANE0_PWR_PRESENT);

	dwc_wr(DCTL, dwc_rd(DCTL) | DCTL_RUN_STOP);
	for (i = 0; i < 500 && (dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++)
		ud_delay_us(1000);
	if (dwc_rd(DSTS) & DSTS_DEVCTRLHLT)
		return -5;
	ud.running = true;
	return 0;
}

void usbdev_poll(void)
{
	uint32_t count;

	if (!ud.running)
		return;
	count = dwc_rd(GEVNTCOUNT) & 0xffff;
	for (uint32_t done = 0; done < count; done += 4) {
		uint32_t ev = *(volatile uint32_t *)(ud.dma + DMA_EVT + ud.evt_pos);

		ud.evt_pos = (ud.evt_pos + 4) % EVT_SIZE;
		ud.events++;
		ud_event(ev);
	}
	if (count)
		dwc_wr(GEVNTCOUNT, count);
}

void usbdev_stop(void)
{
	uint32_t i;

	if (!ud.touched)
		return;
	ud.running = false;
	ud_end_transfer(EP_RX);
	ud_end_transfer(EP_TX);
	ud_end_transfer(0);
	ud_end_transfer(1);
	ud.configured = false;
	dwc_wr(DCTL, dwc_rd(DCTL) & ~DCTL_RUN_STOP);
	for (i = 0; i < 500 && !(dwc_rd(DSTS) & DSTS_DEVCTRLHLT); i++)
		ud_delay_us(1000);
	dwc_wr(GEVNTCOUNT, dwc_rd(GEVNTCOUNT) & 0xffff);	/* drop unread events */
	/* the ABL values: device mode, halted, ABL's event buffer and PHY overrides */
	dwc_wr(DEVTEN, ud.abl_devten);
	dwc_wr(DALEPENA, ud.abl_dalepena);
	dwc_wr(DCFG, ud.abl_dcfg);
	dwc_wr(GEVNTADRLO, ud.abl_gevntadrlo);
	dwc_wr(GEVNTADRHI, ud.abl_gevntadrhi);
	dwc_wr(GEVNTSIZ, ud.abl_gevntsiz);
	mmio_wr(QSCRATCH_BASE + QSCRATCH_HS_PHY_CTRL, ud.abl_qs_hs);
	mmio_wr(QSCRATCH_BASE + QSCRATCH_SS_PHY_CTRL, ud.abl_qs_ss);
	ud.touched = false;
}

int usbdev_configured(void)
{
	return ud.configured ? 1 : 0;
}

uint32_t usbdev_state_bits(void)
{
	return (ud.reset_seen ? SB_RESET : 0) | (ud.connected ? SB_CONNECT : 0) |
	       (ud.hs ? SB_HS : 0) | (ud.addressed ? SB_ADDR : 0) | (ud.configured ? SB_CONF : 0);
}

void usbdev_dma_window(uintptr_t *lo, uintptr_t *hi)
{
	*lo = (uintptr_t)usb_dma;
	*hi = (uintptr_t)usb_dma + sizeof(usb_dma);
}

uint32_t usbdev_stat(unsigned which)
{
	switch (which) {
	case 0: return ud.events;
	case 1: return ud.setups;
	case 2: return ud.rx_xfers;
	case 3: return ud.epcmd_err;
	case 4: return (uint32_t)ud.ep0;
	case 5: return ud.touched ? dwc_rd(DSTS) : 0;
	default: return 0;
	}
}
