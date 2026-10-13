/*
 * Copyright The Zephyr Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * USB device controller driver for the high-speed USB OTG controller of the
 * TI OMAP3 SoCs: a Mentor Graphics MUSB core with a ULPI PHY, used in
 * peripheral mode. Transfers use the endpoint FIFOs, without DMA.
 *
 * The interrupt handler collects the clear-on-read interrupt status and a
 * driver thread handles it under the UDC lock, together with the transfers
 * the stack queues.
 */

#define DT_DRV_COMPAT ti_omap3_musb

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "udc_common.h"

LOG_MODULE_REGISTER(udc_omap3_musb, CONFIG_UDC_DRIVER_LOG_LEVEL);

/* MUSB core registers */
#define MUSB_FADDR     0x00U
#define MUSB_POWER     0x01U
#define MUSB_INTRTX    0x02U
#define MUSB_INTRRX    0x04U
#define MUSB_INTRTXE   0x06U
#define MUSB_INTRRXE   0x08U
#define MUSB_INTRUSB   0x0AU
#define MUSB_INTRUSBE  0x0BU
#define MUSB_INDEX     0x0EU
#define MUSB_TXMAXP    0x10U
#define MUSB_CSR0      0x12U
#define MUSB_TXCSR     0x12U
#define MUSB_RXMAXP    0x14U
#define MUSB_RXCSR     0x16U
#define MUSB_COUNT0    0x18U
#define MUSB_RXCOUNT   0x18U
#define MUSB_FIFO(n)   (0x20U + ((n) * 4U))
#define MUSB_DEVCTL    0x60U
#define MUSB_TXFIFOSZ  0x62U
#define MUSB_RXFIFOSZ  0x63U
#define MUSB_TXFIFOADD 0x64U
#define MUSB_RXFIFOADD 0x66U

#define POWER_SUSPENDM BIT(1)
#define POWER_HSMODE   BIT(4)
#define POWER_HSENAB   BIT(5)
#define POWER_SOFTCONN BIT(6)

#define INTRUSB_SUSPEND BIT(0)
#define INTRUSB_RESUME  BIT(1)
#define INTRUSB_RESET   BIT(2)
#define INTRUSB_DISCON  BIT(5)

#define DEVCTL_SESSION BIT(0)

#define CSR0_RXPKTRDY    BIT(0)
#define CSR0_TXPKTRDY    BIT(1)
#define CSR0_SENTSTALL   BIT(2)
#define CSR0_DATAEND     BIT(3)
#define CSR0_SETUPEND    BIT(4)
#define CSR0_SENDSTALL   BIT(5)
#define CSR0_SVDRXPKTRDY BIT(6)
#define CSR0_SVDSETUPEND BIT(7)

#define TXCSR_TXPKTRDY   BIT(0)
#define TXCSR_UNDERRUN   BIT(2)
#define TXCSR_FLUSHFIFO  BIT(3)
#define TXCSR_SENDSTALL  BIT(4)
#define TXCSR_SENTSTALL  BIT(5)
#define TXCSR_CLRDATATOG BIT(6)
#define TXCSR_MODE       BIT(13)

#define RXCSR_RXPKTRDY   BIT(0)
#define RXCSR_OVERRUN    BIT(2)
#define RXCSR_FLUSHFIFO  BIT(4)
#define RXCSR_SENDSTALL  BIT(5)
#define RXCSR_SENTSTALL  BIT(6)
#define RXCSR_CLRDATATOG BIT(7)

/* OMAP OTG wrapper registers */
#define OTG_SYSCONFIG  0x404U
#define OTG_SYSSTATUS  0x408U
#define OTG_INTERFSEL  0x40CU
#define OTG_FORCESTDBY 0x414U

#define SYSCONFIG_SOFTRESET  BIT(1)
#define SYSCONFIG_NOIDLE     (1U << 3)
#define SYSCONFIG_NOSTANDBY  (1U << 12)
#define SYSSTATUS_RESETDONE  BIT(0)
#define INTERFSEL_ULPI_12PIN BIT(0)

#define EP0_MPS        64U
/* FIFO RAM: EP0 uses the first 64 bytes, then 512 bytes per direction */
#define FIFO_SLOT      512U
#define FIFO_SLOT_CODE 6U /* 8 << 6 bytes */
#define FIFO_BASE      64U

#define RESET_TIMEOUT_US 10000U

enum musb_event {
	MUSB_EVT_IRQ,
	MUSB_EVT_XFER,
};

enum ep0_state {
	EP0_IDLE,
	EP0_SETUP,
	EP0_IN_DATA,
	EP0_OUT_DATA,
	EP0_STATUS,
};

struct musb_config {
	mm_reg_t base;
	size_t num_of_eps;
	struct udc_ep_config *ep_cfg_in;
	struct udc_ep_config *ep_cfg_out;
	struct gpio_dt_spec phy_reset;
	void (*irq_config)(void);
	k_thread_stack_t *stack;
	size_t stack_size;
	int speed_idx;
};

struct musb_data {
	struct k_thread thread;
	struct k_event events;
	const struct device *dev;
	atomic_t intr_usb;
	atomic_t intr_tx;
	atomic_t intr_rx;
	atomic_t xfer_new;
	enum ep0_state ep0;
	/* SETUP read but not acknowledged yet */
	bool ep0_ackpend;
	/* DATAEND given, the controller runs the status stage */
	bool ep0_dataend;
	/* Status stage done before the stack queued the status buffer */
	bool ep0_status_done;
	uint16_t in_chunk[16];
	uint8_t setup[8];
};

/* High speed when both the devicetree and the stack allow it */
static bool musb_hs(const struct device *dev)
{
	const struct musb_config *config = dev->config;

	return IS_ENABLED(CONFIG_UDC_DRIVER_HIGH_SPEED_SUPPORT_ENABLED) && (config->speed_idx == 2);
}

static uint8_t rd8(const struct device *dev, uint32_t reg)
{
	const struct musb_config *config = dev->config;

	return sys_read8(config->base + reg);
}

static void wr8(const struct device *dev, uint32_t reg, uint8_t val)
{
	const struct musb_config *config = dev->config;

	sys_write8(val, config->base + reg);
}

static uint16_t rd16(const struct device *dev, uint32_t reg)
{
	const struct musb_config *config = dev->config;

	return sys_read16(config->base + reg);
}

static void wr16(const struct device *dev, uint32_t reg, uint16_t val)
{
	const struct musb_config *config = dev->config;

	sys_write16(val, config->base + reg);
}

static uint32_t rd32(const struct device *dev, uint32_t reg)
{
	const struct musb_config *config = dev->config;

	return sys_read32(config->base + reg);
}

static void wr32(const struct device *dev, uint32_t reg, uint32_t val)
{
	const struct musb_config *config = dev->config;

	sys_write32(val, config->base + reg);
}

static void select_ep(const struct device *dev, uint8_t idx)
{
	wr8(dev, MUSB_INDEX, idx);
}

static void fifo_write(const struct device *dev, uint8_t idx, const uint8_t *src, size_t len)
{
	size_t i = 0U;

	for (; (i + 4U) <= len; i += 4U) {
		wr32(dev, MUSB_FIFO(idx), sys_get_le32(&src[i]));
	}
	for (; i < len; i++) {
		wr8(dev, MUSB_FIFO(idx), src[i]);
	}
}

static void fifo_read(const struct device *dev, uint8_t idx, uint8_t *dst, size_t len)
{
	size_t i = 0U;

	for (; (i + 4U) <= len; i += 4U) {
		sys_put_le32(rd32(dev, MUSB_FIFO(idx)), &dst[i]);
	}
	for (; i < len; i++) {
		dst[i] = rd8(dev, MUSB_FIFO(idx));
	}
}

static void fifo_discard(const struct device *dev, uint8_t idx, size_t len)
{
	for (size_t i = 0U; i < len; i++) {
		(void)rd8(dev, MUSB_FIFO(idx));
	}
}

/* Endpoint bitmap: OUT endpoints in bits 0-15, IN endpoints in bits 16-31 */
static uint32_t ep_bit(uint8_t ep)
{
	return BIT((USB_EP_DIR_IS_IN(ep) ? 16U : 0U) + USB_EP_GET_IDX(ep));
}

static uint8_t ep_pull(uint32_t *bitmap)
{
	unsigned int bit = find_lsb_set(*bitmap) - 1U;

	*bitmap &= ~BIT(bit);

	return (bit >= 16U) ? (USB_EP_DIR_IN | (bit - 16U)) : (USB_EP_DIR_OUT | bit);
}

static void post_xfer(const struct device *dev, uint8_t ep)
{
	struct musb_data *priv = udc_get_private(dev);

	atomic_or(&priv->xfer_new, ep_bit(ep));
	k_event_post(&priv->events, BIT(MUSB_EVT_XFER));
}

/* Completes the head buffer of an endpoint */
static void ep_complete(const struct device *dev, uint8_t ep, int err)
{
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct net_buf *buf = udc_buf_get(cfg);

	udc_ep_set_busy(cfg, false);
	if (buf != NULL) {
		udc_submit_ep_event(dev, buf, err);
	}
}

/* ---------- control endpoint ---------- */

static void ep0_write_csr(const struct device *dev, uint16_t bits)
{
	struct musb_data *priv = udc_get_private(dev);

	if (priv->ep0_ackpend) {
		bits |= CSR0_SVDRXPKTRDY;
		priv->ep0_ackpend = false;
	}

	select_ep(dev, 0U);
	wr16(dev, MUSB_CSR0, bits);
}

/* Completes the status buffer once both it and the handshake are there */
static void ep0_try_complete_status(const struct device *dev)
{
	struct musb_data *priv = udc_get_private(dev);
	const uint8_t eps[] = {USB_CONTROL_EP_IN, USB_CONTROL_EP_OUT};

	if (!priv->ep0_status_done) {
		return;
	}

	for (size_t i = 0U; i < ARRAY_SIZE(eps); i++) {
		struct net_buf *buf = udc_buf_peek(udc_get_ep_cfg(dev, eps[i]));

		if ((buf != NULL) && udc_get_buf_info(buf)->status) {
			priv->ep0_status_done = false;
			ep_complete(dev, eps[i], 0);
			return;
		}
	}
}

static void ep0_status(const struct device *dev)
{
	struct musb_data *priv = udc_get_private(dev);

	if (!priv->ep0_dataend) {
		ep0_write_csr(dev, CSR0_DATAEND);
		priv->ep0_dataend = true;
	}
	priv->ep0 = EP0_STATUS;
	ep0_try_complete_status(dev);
}

static void ep0_in_next(const struct device *dev, struct net_buf *buf)
{
	struct musb_data *priv = udc_get_private(dev);
	size_t len = MIN(buf->len, EP0_MPS);
	bool last = (len < EP0_MPS) || ((buf->len == len) && !udc_ep_buf_has_zlp(buf));

	if ((buf->len == 0U) && udc_ep_buf_has_zlp(buf)) {
		udc_ep_buf_clear_zlp(buf);
		last = true;
	}

	select_ep(dev, 0U);
	if (priv->ep0_ackpend) {
		/* EP0 has one FIFO: release the SETUP before loading the data */
		wr16(dev, MUSB_CSR0, CSR0_SVDRXPKTRDY);
		priv->ep0_ackpend = false;
		if (!WAIT_FOR((rd16(dev, MUSB_CSR0) & CSR0_RXPKTRDY) == 0U, 1000, k_busy_wait(1))) {
			LOG_WRN("SETUP not released");
		}
	}
	fifo_write(dev, 0U, buf->data, len);
	priv->in_chunk[0] = len;

	if (last) {
		ep0_write_csr(dev, CSR0_TXPKTRDY | CSR0_DATAEND);
		priv->ep0_dataend = true;
		priv->ep0 = EP0_STATUS;
		/* The data stage is over once the last packet is in the FIFO */
		net_buf_pull(buf, len);
		ep_complete(dev, USB_CONTROL_EP_IN, 0);
	} else {
		ep0_write_csr(dev, CSR0_TXPKTRDY);
		priv->ep0 = EP0_IN_DATA;
	}
}

static void ep0_out_read(const struct device *dev)
{
	struct musb_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(udc_get_ep_cfg(dev, USB_CONTROL_EP_OUT));
	uint16_t count;
	size_t len;

	select_ep(dev, 0U);
	count = rd16(dev, MUSB_COUNT0);
	if (buf == NULL) {
		return;
	}

	len = MIN(count, net_buf_tailroom(buf));
	fifo_read(dev, 0U, net_buf_add(buf, len), len);
	fifo_discard(dev, 0U, count - len);

	if ((count < EP0_MPS) || (net_buf_tailroom(buf) == 0U)) {
		wr16(dev, MUSB_CSR0, CSR0_SVDRXPKTRDY | CSR0_DATAEND);
		priv->ep0_dataend = true;
		priv->ep0 = EP0_STATUS;
		ep_complete(dev, USB_CONTROL_EP_OUT, 0);
	} else {
		wr16(dev, MUSB_CSR0, CSR0_SVDRXPKTRDY);
	}
}

static void ep0_setup(const struct device *dev)
{
	struct musb_data *priv = udc_get_private(dev);
	uint16_t count;

	select_ep(dev, 0U);
	count = rd16(dev, MUSB_COUNT0);
	if (count != sizeof(priv->setup)) {
		LOG_WRN("SETUP of %u bytes", count);
		fifo_discard(dev, 0U, count);
		wr16(dev, MUSB_CSR0, CSR0_SVDRXPKTRDY | CSR0_SENDSTALL);
		return;
	}

	fifo_read(dev, 0U, priv->setup, sizeof(priv->setup));
	LOG_DBG("SETUP %02x %02x %02x%02x %02x%02x %02x%02x", priv->setup[0], priv->setup[1],
		priv->setup[3], priv->setup[2], priv->setup[5], priv->setup[4], priv->setup[7],
		priv->setup[6]);
	priv->ep0 = EP0_SETUP;
	priv->ep0_ackpend = true;
	priv->ep0_dataend = false;
	priv->ep0_status_done = false;

	udc_setup_received(dev, priv->setup);
}

static void ep0_irq(const struct device *dev)
{
	struct musb_data *priv = udc_get_private(dev);
	uint16_t csr;

	select_ep(dev, 0U);
	csr = rd16(dev, MUSB_CSR0);

	if ((csr & CSR0_SENTSTALL) != 0U) {
		wr16(dev, MUSB_CSR0, csr & ~CSR0_SENTSTALL);
		priv->ep0 = EP0_IDLE;
		csr = rd16(dev, MUSB_CSR0);
	}

	if ((csr & CSR0_SETUPEND) != 0U) {
		/* The host ended the transfer early, before a new SETUP */
		wr16(dev, MUSB_CSR0, CSR0_SVDSETUPEND);
		priv->ep0 = EP0_IDLE;
		priv->ep0_ackpend = false;
		csr = rd16(dev, MUSB_CSR0);
	}

	switch (priv->ep0) {
	case EP0_STATUS:
		/* The interrupt after DATAEND marks the end of the status stage */
		priv->ep0 = EP0_IDLE;
		priv->ep0_status_done = true;
		ep0_try_complete_status(dev);
		break;
	case EP0_IN_DATA: {
		struct net_buf *buf = udc_buf_peek(udc_get_ep_cfg(dev, USB_CONTROL_EP_IN));

		if (buf != NULL) {
			net_buf_pull(buf, priv->in_chunk[0]);
			ep0_in_next(dev, buf);
		}
		break;
	}
	case EP0_OUT_DATA:
		if ((csr & CSR0_RXPKTRDY) != 0U) {
			ep0_out_read(dev);
		}
		return;
	default:
		break;
	}

	if ((priv->ep0 == EP0_IDLE) && ((csr & CSR0_RXPKTRDY) != 0U)) {
		ep0_setup(dev);
	}
}

static void ep0_start(const struct device *dev, uint8_t ep)
{
	struct musb_data *priv = udc_get_private(dev);
	struct net_buf *buf = udc_buf_peek(udc_get_ep_cfg(dev, ep));
	struct udc_buf_info *bi;

	if (buf == NULL) {
		return;
	}

	bi = udc_get_buf_info(buf);
	if (bi->setup) {
		/* SETUP packets arrive without any action */
		return;
	}

	if (bi->status) {
		/*
		 * The stack queues the status buffer with the data stage
		 * buffer; the status stage only starts after the data stage.
		 */
		if ((priv->ep0 == EP0_STATUS) || priv->ep0_status_done) {
			ep0_try_complete_status(dev);
		} else if (USB_EP_DIR_IS_IN(ep) && (priv->ep0 == EP0_SETUP)) {
			/* No data stage */
			ep0_status(dev);
		}
		return;
	}

	if (priv->ep0 != EP0_SETUP) {
		return;
	}

	if (USB_EP_DIR_IS_IN(ep)) {
		ep0_in_next(dev, buf);
	} else {
		priv->ep0 = EP0_OUT_DATA;
		ep0_write_csr(dev, 0U);
	}
}

/* ---------- other endpoints ---------- */

static void in_next(const struct device *dev, uint8_t ep, struct net_buf *buf)
{
	struct musb_data *priv = udc_get_private(dev);
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	uint8_t idx = USB_EP_GET_IDX(ep);
	size_t len = MIN(buf->len, udc_mps_ep_size(cfg));
	uint16_t csr;

	select_ep(dev, idx);
	fifo_write(dev, idx, buf->data, len);
	priv->in_chunk[idx] = len;
	csr = rd16(dev, MUSB_TXCSR) & ~(TXCSR_UNDERRUN | TXCSR_SENTSTALL);
	wr16(dev, MUSB_TXCSR, csr | TXCSR_TXPKTRDY);
	udc_ep_set_busy(cfg, true);
}

static void in_irq(const struct device *dev, uint8_t idx)
{
	struct musb_data *priv = udc_get_private(dev);
	uint8_t ep = USB_EP_DIR_IN | idx;
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct net_buf *buf;
	uint16_t csr;

	select_ep(dev, idx);
	csr = rd16(dev, MUSB_TXCSR);
	if ((csr & TXCSR_SENTSTALL) != 0U) {
		wr16(dev, MUSB_TXCSR, csr & ~TXCSR_SENTSTALL);
		return;
	}

	buf = udc_buf_peek(cfg);
	if ((buf == NULL) || !udc_ep_is_busy(cfg)) {
		return;
	}

	net_buf_pull(buf, priv->in_chunk[idx]);
	if (buf->len > 0U) {
		in_next(dev, ep, buf);
	} else if (udc_ep_buf_has_zlp(buf)) {
		udc_ep_buf_clear_zlp(buf);
		in_next(dev, ep, buf);
	} else {
		ep_complete(dev, ep, 0);
		post_xfer(dev, ep);
	}
}

/* Reads a packet waiting in the FIFO into the head buffer, if any */
static void out_read(const struct device *dev, uint8_t idx)
{
	uint8_t ep = USB_EP_DIR_OUT | idx;
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct net_buf *buf;
	uint16_t csr;
	uint16_t count;
	size_t len;

	select_ep(dev, idx);
	csr = rd16(dev, MUSB_RXCSR);
	if ((csr & RXCSR_SENTSTALL) != 0U) {
		wr16(dev, MUSB_RXCSR, csr & ~RXCSR_SENTSTALL);
		return;
	}

	buf = udc_buf_peek(cfg);
	if (((csr & RXCSR_RXPKTRDY) == 0U) || (buf == NULL) || cfg->stat.halted) {
		/* Left in the FIFO, the host is NAKed until a buffer is queued */
		return;
	}

	count = rd16(dev, MUSB_RXCOUNT);
	len = MIN(count, net_buf_tailroom(buf));
	fifo_read(dev, idx, net_buf_add(buf, len), len);
	fifo_discard(dev, idx, count - len);
	wr16(dev, MUSB_RXCSR, csr & ~(RXCSR_RXPKTRDY | RXCSR_OVERRUN));

	if ((count < udc_mps_ep_size(cfg)) || (net_buf_tailroom(buf) == 0U)) {
		ep_complete(dev, ep, 0);
	}
}

static void ep_start(const struct device *dev, uint8_t ep)
{
	struct udc_ep_config *cfg = udc_get_ep_cfg(dev, ep);
	struct net_buf *buf;

	if (USB_EP_GET_IDX(ep) == 0U) {
		ep0_start(dev, ep);
		return;
	}

	if (USB_EP_DIR_IS_OUT(ep)) {
		out_read(dev, USB_EP_GET_IDX(ep));
		return;
	}

	buf = udc_buf_peek(cfg);
	if ((buf != NULL) && !udc_ep_is_busy(cfg) && !cfg->stat.halted) {
		in_next(dev, ep, buf);
	}
}

/* ---------- bus events and thread ---------- */

static void usb_irq(const struct device *dev, uint8_t intr)
{
	struct musb_data *priv = udc_get_private(dev);

	if (((intr & INTRUSB_SUSPEND) != 0U) && ((intr & INTRUSB_RESET) == 0U)) {
		udc_set_suspended(dev, true);
		udc_submit_event(dev, UDC_EVT_SUSPEND, 0);
	}

	if ((intr & INTRUSB_RESET) != 0U) {
		priv->ep0 = EP0_IDLE;
		priv->ep0_ackpend = false;
		wr8(dev, MUSB_FADDR, 0U);
		/* The bus idles before the first reset, which raises a suspend */
		udc_set_suspended(dev, false);
		LOG_DBG("reset, %s speed",
			((rd8(dev, MUSB_POWER) & POWER_HSMODE) != 0U) ? "high" : "full");
		udc_submit_event(dev, UDC_EVT_RESET, 0);
	}

	if ((intr & INTRUSB_RESUME) != 0U) {
		udc_set_suspended(dev, false);
		udc_submit_event(dev, UDC_EVT_RESUME, 0);
	}
}

static void musb_thread(void *arg1, void *arg2, void *arg3)
{
	const struct device *dev = arg1;
	struct musb_data *priv = udc_get_private(dev);

	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	while (true) {
		uint32_t evt = k_event_wait(&priv->events, UINT32_MAX, false, K_FOREVER);
		uint32_t tx;
		uint32_t rx;
		uint32_t eps;

		k_event_clear(&priv->events, evt);
		udc_lock_internal(dev, K_FOREVER);

		usb_irq(dev, (uint8_t)atomic_clear(&priv->intr_usb));

		tx = atomic_clear(&priv->intr_tx);
		rx = atomic_clear(&priv->intr_rx);
		if ((tx & BIT(0)) != 0U) {
			ep0_irq(dev);
		}
		for (uint8_t i = 1U; i < 16U; i++) {
			if ((tx & BIT(i)) != 0U) {
				in_irq(dev, i);
			}
			if ((rx & BIT(i)) != 0U) {
				out_read(dev, i);
			}
		}

		eps = atomic_clear(&priv->xfer_new);
		while (eps != 0U) {
			ep_start(dev, ep_pull(&eps));
		}

		udc_unlock_internal(dev);
	}
}

static void musb_isr(const struct device *dev)
{
	struct musb_data *priv = udc_get_private(dev);

	/* Reading clears the interrupt status */
	atomic_or(&priv->intr_usb, rd8(dev, MUSB_INTRUSB));
	atomic_or(&priv->intr_tx, rd16(dev, MUSB_INTRTX));
	atomic_or(&priv->intr_rx, rd16(dev, MUSB_INTRRX));
	k_event_post(&priv->events, BIT(MUSB_EVT_IRQ));
}

/* ---------- UDC API ---------- */

static int musb_ep_enqueue(const struct device *dev, struct udc_ep_config *const cfg,
			   struct net_buf *buf)
{
	udc_buf_put(cfg, buf);
	post_xfer(dev, cfg->addr);

	return 0;
}

static int musb_ep_dequeue(const struct device *dev, struct udc_ep_config *const cfg)
{
	uint8_t idx = USB_EP_GET_IDX(cfg->addr);

	if (idx != 0U) {
		select_ep(dev, idx);
		if (USB_EP_DIR_IS_IN(cfg->addr)) {
			wr16(dev, MUSB_TXCSR, TXCSR_MODE | TXCSR_FLUSHFIFO);
		} else {
			wr16(dev, MUSB_RXCSR, RXCSR_FLUSHFIFO);
		}
	}

	udc_ep_cancel_queued(dev, cfg);
	udc_ep_set_busy(cfg, false);

	return 0;
}

static int musb_ep_enable(const struct device *dev, struct udc_ep_config *const cfg)
{
	uint8_t idx = USB_EP_GET_IDX(cfg->addr);
	uint16_t addr = (FIFO_BASE + ((idx - 1U) * 2U * FIFO_SLOT)) / 8U;

	if (idx == 0U) {
		return 0;
	}

	select_ep(dev, idx);
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		wr8(dev, MUSB_TXFIFOSZ, FIFO_SLOT_CODE);
		wr16(dev, MUSB_TXFIFOADD, addr);
		wr16(dev, MUSB_TXMAXP, udc_mps_ep_size(cfg));
		wr16(dev, MUSB_TXCSR, TXCSR_MODE | TXCSR_CLRDATATOG | TXCSR_FLUSHFIFO);
		wr16(dev, MUSB_INTRTXE, rd16(dev, MUSB_INTRTXE) | BIT(idx));
	} else {
		wr8(dev, MUSB_RXFIFOSZ, FIFO_SLOT_CODE);
		wr16(dev, MUSB_RXFIFOADD, addr + (FIFO_SLOT / 8U));
		wr16(dev, MUSB_RXMAXP, udc_mps_ep_size(cfg));
		wr16(dev, MUSB_RXCSR, RXCSR_CLRDATATOG | RXCSR_FLUSHFIFO);
		wr16(dev, MUSB_INTRRXE, rd16(dev, MUSB_INTRRXE) | BIT(idx));
	}

	return 0;
}

static int musb_ep_disable(const struct device *dev, struct udc_ep_config *const cfg)
{
	uint8_t idx = USB_EP_GET_IDX(cfg->addr);

	if (idx == 0U) {
		return 0;
	}

	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		wr16(dev, MUSB_INTRTXE, rd16(dev, MUSB_INTRTXE) & ~BIT(idx));
	} else {
		wr16(dev, MUSB_INTRRXE, rd16(dev, MUSB_INTRRXE) & ~BIT(idx));
	}

	return musb_ep_dequeue(dev, cfg);
}

static int musb_ep_set_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	uint8_t idx = USB_EP_GET_IDX(cfg->addr);

	if (idx == 0U) {
		struct musb_data *priv = udc_get_private(dev);

		/* Protocol stall, ends with the next SETUP */
		LOG_DBG("EP0 stall");
		ep0_write_csr(dev, CSR0_SENDSTALL);
		priv->ep0 = EP0_IDLE;
		return 0;
	}

	select_ep(dev, idx);
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		wr16(dev, MUSB_TXCSR, rd16(dev, MUSB_TXCSR) | TXCSR_SENDSTALL);
	} else {
		wr16(dev, MUSB_RXCSR, rd16(dev, MUSB_RXCSR) | RXCSR_SENDSTALL);
	}
	cfg->stat.halted = true;

	return 0;
}

static int musb_ep_clear_halt(const struct device *dev, struct udc_ep_config *const cfg)
{
	uint8_t idx = USB_EP_GET_IDX(cfg->addr);
	uint16_t csr;

	select_ep(dev, idx);
	if (USB_EP_DIR_IS_IN(cfg->addr)) {
		csr = rd16(dev, MUSB_TXCSR) & ~(TXCSR_SENDSTALL | TXCSR_SENTSTALL);
		wr16(dev, MUSB_TXCSR, csr | TXCSR_CLRDATATOG);
	} else {
		csr = rd16(dev, MUSB_RXCSR) & ~(RXCSR_SENDSTALL | RXCSR_SENTSTALL);
		wr16(dev, MUSB_RXCSR, csr | RXCSR_CLRDATATOG);
	}
	cfg->stat.halted = false;
	post_xfer(dev, cfg->addr);

	return 0;
}

static int musb_set_address(const struct device *dev, const uint8_t addr)
{
	/* The stack sets the address after the status stage */
	wr8(dev, MUSB_FADDR, addr);

	return 0;
}

static int musb_host_wakeup(const struct device *dev)
{
	return -ENOTSUP;
}

static enum udc_bus_speed musb_device_speed(const struct device *dev)
{
	return ((rd8(dev, MUSB_POWER) & POWER_HSMODE) != 0U) ? UDC_BUS_SPEED_HS : UDC_BUS_SPEED_FS;
}

static int musb_enable(const struct device *dev)
{
	wr8(dev, MUSB_DEVCTL, rd8(dev, MUSB_DEVCTL) | DEVCTL_SESSION);
	wr8(dev, MUSB_POWER, POWER_SOFTCONN | (musb_hs(dev) ? POWER_HSENAB : 0U));

	return 0;
}

static int musb_disable(const struct device *dev)
{
	wr8(dev, MUSB_POWER, rd8(dev, MUSB_POWER) & ~POWER_SOFTCONN);

	return 0;
}

static int musb_init(const struct device *dev)
{
	const struct musb_config *config = dev->config;
	int ret;

	if (config->phy_reset.port != NULL) {
		if (!gpio_is_ready_dt(&config->phy_reset)) {
			return -ENODEV;
		}

		/* PHY out of reset */
		ret = gpio_pin_configure_dt(&config->phy_reset, GPIO_OUTPUT_INACTIVE);
		if (ret != 0) {
			return ret;
		}
		k_msleep(1);
	}

	wr32(dev, OTG_SYSCONFIG, SYSCONFIG_SOFTRESET);
	if (!WAIT_FOR((rd32(dev, OTG_SYSSTATUS) & SYSSTATUS_RESETDONE) != 0U, RESET_TIMEOUT_US,
		      k_busy_wait(10))) {
		LOG_ERR("reset timed out");
		return -ETIMEDOUT;
	}

	wr32(dev, OTG_SYSCONFIG, SYSCONFIG_NOIDLE | SYSCONFIG_NOSTANDBY);
	wr32(dev, OTG_INTERFSEL, INTERFSEL_ULPI_12PIN);
	wr32(dev, OTG_FORCESTDBY, 0U);

	wr8(dev, MUSB_POWER, 0U);
	wr8(dev, MUSB_FADDR, 0U);
	wr16(dev, MUSB_INTRTXE, BIT(0));
	wr16(dev, MUSB_INTRRXE, 0U);
	wr8(dev, MUSB_INTRUSBE, INTRUSB_RESET | INTRUSB_SUSPEND | INTRUSB_RESUME);
	(void)rd8(dev, MUSB_INTRUSB);
	(void)rd16(dev, MUSB_INTRTX);
	(void)rd16(dev, MUSB_INTRRX);

	if (udc_ep_enable_internal(dev, USB_CONTROL_EP_OUT, USB_EP_TYPE_CONTROL, EP0_MPS, 0) ||
	    udc_ep_enable_internal(dev, USB_CONTROL_EP_IN, USB_EP_TYPE_CONTROL, EP0_MPS, 0)) {
		LOG_ERR("Failed to enable control endpoint");
		return -EIO;
	}

	config->irq_config();

	return 0;
}

static int musb_shutdown(const struct device *dev)
{
	wr8(dev, MUSB_POWER, 0U);

	if (udc_ep_disable_internal(dev, USB_CONTROL_EP_OUT) ||
	    udc_ep_disable_internal(dev, USB_CONTROL_EP_IN)) {
		LOG_ERR("Failed to disable control endpoint");
		return -EIO;
	}

	return 0;
}

static void musb_lock(const struct device *dev)
{
	udc_lock_internal(dev, K_FOREVER);
}

static void musb_unlock(const struct device *dev)
{
	udc_unlock_internal(dev);
}

static const struct udc_api musb_api = {
	.lock = musb_lock,
	.unlock = musb_unlock,
	.device_speed = musb_device_speed,
	.init = musb_init,
	.enable = musb_enable,
	.disable = musb_disable,
	.shutdown = musb_shutdown,
	.set_address = musb_set_address,
	.host_wakeup = musb_host_wakeup,
	.ep_enable = musb_ep_enable,
	.ep_disable = musb_ep_disable,
	.ep_set_halt = musb_ep_set_halt,
	.ep_clear_halt = musb_ep_clear_halt,
	.ep_enqueue = musb_ep_enqueue,
	.ep_dequeue = musb_ep_dequeue,
};

static int musb_register_eps(const struct device *dev)
{
	const struct musb_config *config = dev->config;
	uint16_t mps = musb_hs(dev) ? 512U : 64U;
	int err;

	for (size_t i = 0U; i < config->num_of_eps; i++) {
		struct udc_ep_config *out = &config->ep_cfg_out[i];
		struct udc_ep_config *in = &config->ep_cfg_in[i];

		out->caps.out = 1;
		in->caps.in = 1;
		if (i == 0U) {
			out->caps.control = 1;
			in->caps.control = 1;
			out->caps.mps = EP0_MPS;
			in->caps.mps = EP0_MPS;
		} else {
			out->caps.bulk = 1;
			out->caps.interrupt = 1;
			in->caps.bulk = 1;
			in->caps.interrupt = 1;
			out->caps.mps = mps;
			in->caps.mps = mps;
		}
		out->addr = USB_EP_DIR_OUT | i;
		in->addr = USB_EP_DIR_IN | i;

		err = udc_register_ep(dev, out);
		if (err == 0) {
			err = udc_register_ep(dev, in);
		}
		if (err != 0) {
			LOG_ERR("Failed to register endpoint");
			return err;
		}
	}

	return 0;
}

static int musb_driver_preinit(const struct device *dev)
{
	const struct musb_config *config = dev->config;
	struct musb_data *priv = udc_get_private(dev);
	struct udc_data *data = dev->data;
	int err;

	k_mutex_init(&data->mutex);
	k_event_init(&priv->events);
	priv->dev = dev;

	data->caps.mps0 = UDC_MPS0_64;
	data->caps.hs = musb_hs(dev);

	err = musb_register_eps(dev);
	if (err != 0) {
		return err;
	}

	k_thread_create(&priv->thread, config->stack, config->stack_size, musb_thread, (void *)dev,
			NULL, NULL, K_PRIO_COOP(CONFIG_UDC_OMAP3_MUSB_THREAD_PRIORITY), K_ESSENTIAL,
			K_NO_WAIT);
	k_thread_name_set(&priv->thread, dev->name);

	return 0;
}

#define MUSB_SPEED_IDX(n)                                                                          \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, maximum_speed),                                       \
		    (DT_ENUM_IDX(DT_DRV_INST(n), maximum_speed)), (2))

#define MUSB_PHY_RESET(n)                                                                          \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(n, phys),                                                \
		    (GPIO_DT_SPEC_GET_OR(DT_INST_PHANDLE(n, phys), reset_gpios, {0})), ({0}))

#define MUSB_DEVICE_DEFINE(n)                                                                      \
	BUILD_ASSERT(DT_INST_PROP(n, num_bidir_endpoints) <= 16, "at most 16 endpoints");          \
	K_THREAD_STACK_DEFINE(musb_stack_##n, CONFIG_UDC_OMAP3_MUSB_STACK_SIZE);                   \
                                                                                                   \
	static void musb_irq_config_##n(void)                                                      \
	{                                                                                          \
		IRQ_CONNECT(DT_INST_IRQN(n), DT_INST_IRQ(n, priority), musb_isr,                   \
			    DEVICE_DT_INST_GET(n), 0);                                             \
		irq_enable(DT_INST_IRQN(n));                                                       \
	}                                                                                          \
                                                                                                   \
	static struct udc_ep_config ep_cfg_out_##n[DT_INST_PROP(n, num_bidir_endpoints)];          \
	static struct udc_ep_config ep_cfg_in_##n[DT_INST_PROP(n, num_bidir_endpoints)];           \
                                                                                                   \
	static const struct musb_config musb_config_##n = {                                        \
		.base = DT_INST_REG_ADDR(n),                                                       \
		.num_of_eps = DT_INST_PROP(n, num_bidir_endpoints),                                \
		.ep_cfg_in = ep_cfg_in_##n,                                                        \
		.ep_cfg_out = ep_cfg_out_##n,                                                      \
		.phy_reset = MUSB_PHY_RESET(n),                                                    \
		.irq_config = musb_irq_config_##n,                                                 \
		.stack = musb_stack_##n,                                                           \
		.stack_size = K_THREAD_STACK_SIZEOF(musb_stack_##n),                               \
		.speed_idx = MUSB_SPEED_IDX(n),                                                    \
	};                                                                                         \
                                                                                                   \
	static struct musb_data musb_priv_##n;                                                     \
                                                                                                   \
	static struct udc_data udc_data_##n = {                                                    \
		.mutex = Z_MUTEX_INITIALIZER(udc_data_##n.mutex),                                  \
		.priv = &musb_priv_##n,                                                            \
	};                                                                                         \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, musb_driver_preinit, NULL, &udc_data_##n, &musb_config_##n,       \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &musb_api);

DT_INST_FOREACH_STATUS_OKAY(MUSB_DEVICE_DEFINE)
