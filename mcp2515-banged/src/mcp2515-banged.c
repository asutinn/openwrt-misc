/*
 * CAN bus driver for Microchip 251x CAN Controller with SPI Interface
 *
 * MCP2510 support and bug fixes by Christian Pellegrin
 * <chripell@evolware.org>
 *
 * Copyright 2009 Christian Pellegrin EVOL S.r.l.
 *
 * Copyright 2007 Raymarine UK, Ltd. All Rights Reserved.
 * Written under contract by:
 *   Chris Elston, Katalix Systems, Ltd.
 *
 * Based on Microchip MCP251x CAN controller driver written by
 * David Vrabel, Copyright 2006 Arcom Control Systems Ltd.
 *
 * Based on CAN bus driver for the CCAN controller written by
 * - Sascha Hauer, Marc Kleine-Budde, Pengutronix
 * - Simon Kallweit, intefo AG
 * Copyright 2007
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the version 2 of the GNU General Public License
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#include <linux/can/core.h>
#include <linux/can/dev.h>
#include <linux/can/led.h>
#include <linux/can/platform/mcp251x.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/freezer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>

/* SPI interface instruction set */
#define INSTRUCTION_WRITE	0x02
#define INSTRUCTION_READ	0x03
#define INSTRUCTION_BIT_MODIFY	0x05
#define INSTRUCTION_LOAD_TXB(n)	(0x40 + 2 * (n))
#define INSTRUCTION_READ_RXB(n)	(((n) == 0) ? 0x90 : 0x94)
#define INSTRUCTION_RESET	0xC0
#define RTS_TXB0		0x01
#define RTS_TXB1		0x02
#define RTS_TXB2		0x04
#define INSTRUCTION_RTS(n)	(0x80 | ((n) & 0x07))


/* MPC251x registers */
#define CANSTAT	      0x0e
#define CANCTRL	      0x0f
#  define CANCTRL_REQOP_MASK	    0xe0
#  define CANCTRL_REQOP_CONF	    0x80
#  define CANCTRL_REQOP_LISTEN_ONLY 0x60
#  define CANCTRL_REQOP_LOOPBACK    0x40
#  define CANCTRL_REQOP_SLEEP	    0x20
#  define CANCTRL_REQOP_NORMAL	    0x00
#  define CANCTRL_OSM		    0x08
#  define CANCTRL_ABAT		    0x10
#define TEC	      0x1c
#define REC	      0x1d
#define CNF1	      0x2a
#  define CNF1_SJW_SHIFT   6
#define CNF2	      0x29
#  define CNF2_BTLMODE	   0x80
#  define CNF2_SAM         0x40
#  define CNF2_PS1_SHIFT   3
#define CNF3	      0x28
#  define CNF3_SOF	   0x08
#  define CNF3_WAKFIL	   0x04
#  define CNF3_PHSEG2_MASK 0x07
#define CANINTE	      0x2b
#  define CANINTE_MERRE 0x80
#  define CANINTE_WAKIE 0x40
#  define CANINTE_ERRIE 0x20
#  define CANINTE_TX2IE 0x10
#  define CANINTE_TX1IE 0x08
#  define CANINTE_TX0IE 0x04
#  define CANINTE_RX1IE 0x02
#  define CANINTE_RX0IE 0x01
#define CANINTF	      0x2c
#  define CANINTF_MERRF 0x80
#  define CANINTF_WAKIF 0x40
#  define CANINTF_ERRIF 0x20
#  define CANINTF_TX2IF 0x10
#  define CANINTF_TX1IF 0x08
#  define CANINTF_TX0IF 0x04
#  define CANINTF_RX1IF 0x02
#  define CANINTF_RX0IF 0x01
#  define CANINTF_RX (CANINTF_RX0IF | CANINTF_RX1IF)
#  define CANINTF_TX (CANINTF_TX2IF | CANINTF_TX1IF | CANINTF_TX0IF)
#  define CANINTF_ERR (CANINTF_ERRIF)
#define EFLG	      0x2d
#  define EFLG_EWARN	0x01
#  define EFLG_RXWAR	0x02
#  define EFLG_TXWAR	0x04
#  define EFLG_RXEP	0x08
#  define EFLG_TXEP	0x10
#  define EFLG_TXBO	0x20
#  define EFLG_RX0OVR	0x40
#  define EFLG_RX1OVR	0x80
#define TXBCTRL(n)  (((n) * 0x10) + 0x30 + TXBCTRL_OFF)
#  define TXBCTRL_ABTF	0x40
#  define TXBCTRL_MLOA	0x20
#  define TXBCTRL_TXERR 0x10
#  define TXBCTRL_TXREQ 0x08
#define TXBSIDH(n)  (((n) * 0x10) + 0x30 + TXBSIDH_OFF)
#  define SIDH_SHIFT    3
#define TXBSIDL(n)  (((n) * 0x10) + 0x30 + TXBSIDL_OFF)
#  define SIDL_SID_MASK    7
#  define SIDL_SID_SHIFT   5
#  define SIDL_EXIDE_SHIFT 3
#  define SIDL_EID_SHIFT   16
#  define SIDL_EID_MASK    3
#define TXBEID8(n)  (((n) * 0x10) + 0x30 + TXBEID8_OFF)
#define TXBEID0(n)  (((n) * 0x10) + 0x30 + TXBEID0_OFF)
#define TXBDLC(n)   (((n) * 0x10) + 0x30 + TXBDLC_OFF)
#  define DLC_RTR_SHIFT    6
#define TXBCTRL_OFF 0
#define TXBSIDH_OFF 1
#define TXBSIDL_OFF 2
#define TXBEID8_OFF 3
#define TXBEID0_OFF 4
#define TXBDLC_OFF  5
#define TXBDAT_OFF  6
#define RXBCTRL(n)  (((n) * 0x10) + 0x60 + RXBCTRL_OFF)
#  define RXBCTRL_BUKT	0x04
#  define RXBCTRL_RXM0	0x20
#  define RXBCTRL_RXM1	0x40
#define RXBSIDH(n)  (((n) * 0x10) + 0x60 + RXBSIDH_OFF)
#  define RXBSIDH_SHIFT 3
#define RXBSIDL(n)  (((n) * 0x10) + 0x60 + RXBSIDL_OFF)
#  define RXBSIDL_IDE   0x08
#  define RXBSIDL_SRR   0x10
#  define RXBSIDL_EID   3
#  define RXBSIDL_SHIFT 5
#define RXBEID8(n)  (((n) * 0x10) + 0x60 + RXBEID8_OFF)
#define RXBEID0(n)  (((n) * 0x10) + 0x60 + RXBEID0_OFF)
#define RXBDLC(n)   (((n) * 0x10) + 0x60 + RXBDLC_OFF)
#  define RXBDLC_LEN_MASK  0x0f
#  define RXBDLC_RTR       0x40
#define RXBCTRL_OFF 0
#define RXBSIDH_OFF 1
#define RXBSIDL_OFF 2
#define RXBEID8_OFF 3
#define RXBEID0_OFF 4
#define RXBDLC_OFF  5
#define RXBDAT_OFF  6
#define RXFSIDH(n) ((n) * 4)
#define RXFSIDL(n) ((n) * 4 + 1)
#define RXFEID8(n) ((n) * 4 + 2)
#define RXFEID0(n) ((n) * 4 + 3)
#define RXMSIDH(n) ((n) * 4 + 0x20)
#define RXMSIDL(n) ((n) * 4 + 0x21)
#define RXMEID8(n) ((n) * 4 + 0x22)
#define RXMEID0(n) ((n) * 4 + 0x23)

#define GET_BYTE(val, byte)			\
	(((val) >> ((byte) * 8)) & 0xff)
#define SET_BYTE(val, byte)			\
	(((val) & 0xff) << ((byte) * 8))

/*
 * Buffer size required for the largest SPI transfer (i.e., reading a
 * frame)
 */
#define CAN_FRAME_MAX_DATA_LEN	8
#define SPI_TRANSFER_BUF_LEN	(6 + CAN_FRAME_MAX_DATA_LEN)
#define CAN_FRAME_MAX_BITS	128

#define TX_ECHO_SKB_MAX	1

#define MCP251X_OST_DELAY_MS	(5)

#define DEVICE_NAME "mcp2515-banged"

#define GPIO_MISO	0
#define GPIO_MOSI	1
#define GPIO_CLK	2
#define GPIO_CS		3
#define GPIO_INT	4

static int gpios [] = {20, 19, 18, 7, 6};
static int gpio_count;
module_param_array(gpios, int, &gpio_count, 0);
MODULE_PARM_DESC(gpios, "used GPIOS for MISO, MOSI, CLK, CS and INT");


static const struct can_bittiming_const mcp2515_bittiming_const = {
	.name = DEVICE_NAME,
	.tseg1_min = 3,
	.tseg1_max = 16,
	.tseg2_min = 2,
	.tseg2_max = 8,
	.sjw_max = 4,
	.brp_min = 1,
	.brp_max = 64,
	.brp_inc = 1,
};

enum mcp2515_model {
	CAN_MCP251X_MCP2515	= 0x2515,
};

struct mcp2515_priv {
	struct can_priv	   can;
	struct net_device *net;
	struct spi_device *spi;
	enum mcp2515_model model;

	struct mutex mcp_lock; /* SPI device lock */

	u8 *spi_tx_buf;
	u8 *spi_rx_buf;
	int irq;

	struct sk_buff *tx_skb;
	int tx_len;

	struct workqueue_struct *wq;
	struct work_struct tx_work;
	struct work_struct restart_work;

	int force_quit;
	int after_suspend;
#define AFTER_SUSPEND_UP 1
#define AFTER_SUSPEND_DOWN 2
#define AFTER_SUSPEND_POWER 4
#define AFTER_SUSPEND_RESTART 8
	int restart_tx;
	struct regulator *power;
	struct regulator *transceiver;
	struct clk *clk;
};

#define MCP251X_IS(_model) \
static inline int mcp2515_is_##_model(struct spi_device *spi) \
{ \
	struct mcp2515_priv *priv = spi_get_drvdata(spi); \
	return priv->model == CAN_MCP251X_MCP##_model; \
}

MCP251X_IS(2515);

static void mcp2515_clean(struct net_device *net)
{
	struct mcp2515_priv *priv = netdev_priv(net);

	if (priv->tx_skb || priv->tx_len)
		net->stats.tx_errors++;
	if (priv->tx_skb)
		dev_kfree_skb(priv->tx_skb);
	if (priv->tx_len)
		can_free_echo_skb(priv->net, 0);
	priv->tx_skb = NULL;
	priv->tx_len = 0;
}

/*
 * Note about handling of error return of mcp2515_spi_trans: accessing
 * registers via SPI is not really different conceptually than using
 * normal I/O assembler instructions, although it's much more
 * complicated from a practical POV. So it's not advisable to always
 * check the return value of this function. Imagine that every
 * read{b,l}, write{b,l} and friends would be bracketed in "if ( < 0)
 * error();", it would be a great mess (well there are some situation
 * when exception handling C++ like could be useful after all). So we
 * just check that transfers are OK at the beginning of our
 * conversation with the chip and to avoid doing really nasty things
 * (like injecting bogus packets in the network stack).
 */
static int mcp2515_spi_trans(struct mcp2515_priv *priv, int len) {
	int ret;
	int i, j;
	uint8_t data_in, data_out;
	ret = 0;

/*	struct mcp2515_priv *priv = spi_get_drvdata(spi);
	struct spi_transfer t = {
		.tx_buf = priv->spi_tx_buf,
		.rx_buf = priv->spi_rx_buf,
		.len = len,
		.cs_change = 0,
	};
	struct spi_message m;

	spi_message_init(&m);

	spi_message_add_tail(&t, &m);

	ret = spi_sync(spi, &m);
	if (ret)
		dev_err(&spi->dev, "spi transfer failed: ret = %d\n", ret);
*/

	/* MSB first */
	for (i = 0; i < len; i++) {
		data_out = priv->spi_tx_buf[i];
		data_in = 0;
		for (j = 0; j < 8; j++) {
			gpio_set_value(GPIO_CLK, 0);
			if (data_out & 0x80)
				gpio_set_value(GPIO_MOSI,1);
			else
				gpio_set_value(GPIO_MOSI,0);
			gpio_set_value(GPIO_CLK, 1);
			data_in <<= 1;
			if (gpio_get_value(GPIO_MISO))
				data_in |= 0x01;
		}
		priv->spi_rx_buf[i] = data_in;
	}
	printk(KERN_INFO "%s: write ", __func__);
	for (i = 0; i < len; i++)
		printk(KERN_INFO "0x%02x", priv->spi_tx_buf[i]);

	printk(KERN_INFO "\n%s: read ", __func__);
	for (i = 0; i < len; i++)
		printk(KERN_INFO "0x%02x", priv->spi_tx_buf[i]);

	printk(KERN_INFO "\n");

	return ret;
}

static u8 mcp2515_read_reg(struct mcp2515_priv *priv, uint8_t reg) {
	u8 val = 0;

	priv->spi_tx_buf[0] = INSTRUCTION_READ;
	priv->spi_tx_buf[1] = reg;

	gpio_set_value(GPIO_CS, 0);
	mcp2515_spi_trans(priv, 3);
	udelay(1);
	gpio_set_value(GPIO_CS, 1);

	val = priv->spi_rx_buf[2];

	return val;
}

static void mcp2515_read_2regs(struct mcp2515_priv *priv, uint8_t reg, uint8_t *v1, uint8_t *v2) {

	priv->spi_tx_buf[0] = INSTRUCTION_READ;
	priv->spi_tx_buf[1] = reg;

	gpio_set_value(GPIO_CS, 0);
	mcp2515_spi_trans(priv, 4);
	udelay(1);
	gpio_set_value(GPIO_CS, 1);

	*v1 = priv->spi_rx_buf[2];
	*v2 = priv->spi_rx_buf[3];
}

static void mcp2515_write_reg(struct mcp2515_priv *priv, u8 reg, uint8_t val) {

	priv->spi_tx_buf[0] = INSTRUCTION_WRITE;
	priv->spi_tx_buf[1] = reg;
	priv->spi_tx_buf[2] = val;

	gpio_set_value(GPIO_CS, 0);
	mcp2515_spi_trans(priv, 3);
	udelay(1);
	gpio_set_value(GPIO_CS, 1);
}

static void mcp2515_write_bits(struct mcp2515_priv *priv, u8 reg, u8 mask, uint8_t val) {

	priv->spi_tx_buf[0] = INSTRUCTION_BIT_MODIFY;
	priv->spi_tx_buf[1] = reg;
	priv->spi_tx_buf[2] = mask;
	priv->spi_tx_buf[3] = val;

	gpio_set_value(GPIO_CS, 0);
	mcp2515_spi_trans(priv, 4);
	udelay(1);
	gpio_set_value(GPIO_CS, 1);
}

static void mcp2515_hw_tx_frame(struct mcp2515_priv *priv, u8 *buf, int len, int tx_buf_idx) {
	memcpy(priv->spi_tx_buf, buf, TXBDAT_OFF + len);
	mcp2515_spi_trans(priv, TXBDAT_OFF + len);
}

static void mcp2515_hw_tx(struct mcp2515_priv *priv, struct can_frame *frame, int tx_buf_idx) {
	u32 sid, eid, exide, rtr;
	u8 buf[SPI_TRANSFER_BUF_LEN];

	exide = (frame->can_id & CAN_EFF_FLAG) ? 1 : 0; /* Extended ID Enable */
	if (exide)
		sid = (frame->can_id & CAN_EFF_MASK) >> 18;
	else
		sid = frame->can_id & CAN_SFF_MASK; /* Standard ID */
	eid = frame->can_id & CAN_EFF_MASK; /* Extended ID */
	rtr = (frame->can_id & CAN_RTR_FLAG) ? 1 : 0; /* Remote transmission */

	buf[TXBCTRL_OFF] = INSTRUCTION_LOAD_TXB(tx_buf_idx);
	buf[TXBSIDH_OFF] = sid >> SIDH_SHIFT;
	buf[TXBSIDL_OFF] = ((sid & SIDL_SID_MASK) << SIDL_SID_SHIFT) |
		(exide << SIDL_EXIDE_SHIFT) |
		((eid >> SIDL_EID_SHIFT) & SIDL_EID_MASK);
	buf[TXBEID8_OFF] = GET_BYTE(eid, 1);
	buf[TXBEID0_OFF] = GET_BYTE(eid, 0);
	buf[TXBDLC_OFF] = (rtr << DLC_RTR_SHIFT) | frame->can_dlc;
	memcpy(buf + TXBDAT_OFF, frame->data, frame->can_dlc);
	mcp2515_hw_tx_frame(priv, buf, frame->can_dlc, tx_buf_idx);

	/* use INSTRUCTION_RTS, to avoid "repeated frame problem" */
	priv->spi_tx_buf[0] = INSTRUCTION_RTS(1 << tx_buf_idx);
	mcp2515_spi_trans(priv, 1);
}

static void mcp2515_hw_rx_frame(struct mcp2515_priv *priv, u8 *buf, int buf_idx) {

	priv->spi_tx_buf[RXBCTRL_OFF] = INSTRUCTION_READ_RXB(buf_idx);
	mcp2515_spi_trans(priv, SPI_TRANSFER_BUF_LEN);

	memcpy(buf, priv->spi_rx_buf, SPI_TRANSFER_BUF_LEN);
}

static void mcp2515_hw_rx(struct mcp2515_priv *priv, int buf_idx) {
	struct sk_buff *skb;
	struct can_frame *frame;
	u8 buf[SPI_TRANSFER_BUF_LEN];

	skb = alloc_can_skb(priv->net, &frame);
	if (!skb) {
		/* dev_err(&spi->dev, "cannot allocate RX skb\n"); */
		priv->net->stats.rx_dropped++;
		return;
	}

	mcp2515_hw_rx_frame(priv, buf, buf_idx);
	if (buf[RXBSIDL_OFF] & RXBSIDL_IDE) {
		/* Extended ID format */
		frame->can_id = CAN_EFF_FLAG;
		frame->can_id |=
			/* Extended ID part */
			SET_BYTE(buf[RXBSIDL_OFF] & RXBSIDL_EID, 2) |
			SET_BYTE(buf[RXBEID8_OFF], 1) |
			SET_BYTE(buf[RXBEID0_OFF], 0) |
			/* Standard ID part */
			(((buf[RXBSIDH_OFF] << RXBSIDH_SHIFT) |
			  (buf[RXBSIDL_OFF] >> RXBSIDL_SHIFT)) << 18);
		/* Remote transmission request */
		if (buf[RXBDLC_OFF] & RXBDLC_RTR)
			frame->can_id |= CAN_RTR_FLAG;
	} else {
		/* Standard ID format */
		frame->can_id =
			(buf[RXBSIDH_OFF] << RXBSIDH_SHIFT) |
			(buf[RXBSIDL_OFF] >> RXBSIDL_SHIFT);
		if (buf[RXBSIDL_OFF] & RXBSIDL_SRR)
			frame->can_id |= CAN_RTR_FLAG;
	}
	/* Data length */
	frame->can_dlc = get_can_dlc(buf[RXBDLC_OFF] & RXBDLC_LEN_MASK);
	memcpy(frame->data, buf + RXBDAT_OFF, frame->can_dlc);

	priv->net->stats.rx_packets++;
	priv->net->stats.rx_bytes += frame->can_dlc;

	can_led_event(priv->net, CAN_LED_EVENT_RX);

	netif_rx_ni(skb);
}

static void mcp2515_hw_sleep(struct mcp2515_priv *priv)
{
	mcp2515_write_reg(priv, CANCTRL, CANCTRL_REQOP_SLEEP);
}

static netdev_tx_t mcp2515_hard_start_xmit(struct sk_buff *skb, struct net_device *net)
{
	struct mcp2515_priv *priv = netdev_priv(net);

	if (priv->tx_skb || priv->tx_len) {
		/* dev_warn(&spi->dev, "hard_xmit called while tx busy\n"); */
		return NETDEV_TX_BUSY;
	}

	if (can_dropped_invalid_skb(net, skb))
		return NETDEV_TX_OK;

	netif_stop_queue(net);
	priv->tx_skb = skb;
	queue_work(priv->wq, &priv->tx_work);

	return NETDEV_TX_OK;
}

static int mcp2515_do_set_mode(struct net_device *net, enum can_mode mode)
{
	struct mcp2515_priv *priv = netdev_priv(net);

	switch (mode) {
	case CAN_MODE_START:
		mcp2515_clean(net);
		/* We have to delay work since SPI I/O may sleep */
		priv->can.state = CAN_STATE_ERROR_ACTIVE;
		priv->restart_tx = 1;
		if (priv->can.restart_ms == 0)
			priv->after_suspend = AFTER_SUSPEND_RESTART;
		queue_work(priv->wq, &priv->restart_work);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int mcp2515_set_normal_mode(struct net_device *net)
{
	unsigned long timeout;
	struct mcp2515_priv *priv = netdev_priv(net);

	/* Enable interrupts */
	mcp2515_write_reg(priv, CANINTE, CANINTE_ERRIE | CANINTE_TX2IE | CANINTE_TX1IE |
			  CANINTE_TX0IE | CANINTE_RX1IE | CANINTE_RX0IE);

	if (priv->can.ctrlmode & CAN_CTRLMODE_LOOPBACK) {
		/* Put device into loopback mode */
		mcp2515_write_reg(priv, CANCTRL, CANCTRL_REQOP_LOOPBACK);
	} else if (priv->can.ctrlmode & CAN_CTRLMODE_LISTENONLY) {
		/* Put device into listen-only mode */
		mcp2515_write_reg(priv, CANCTRL, CANCTRL_REQOP_LISTEN_ONLY);
	} else {
		/* Put device into normal mode */
		mcp2515_write_reg(priv, CANCTRL, CANCTRL_REQOP_NORMAL);

		/* Wait for the device to enter normal mode */
		timeout = jiffies + HZ;
		while (mcp2515_read_reg(priv, CANSTAT) & CANCTRL_REQOP_MASK) {
			schedule();
			if (time_after(jiffies, timeout)) {
				/* dev_err(&spi->dev, "MCP251x didn't"
					" enter in normal mode\n"); */
				return -EBUSY;
			}
		}
	}
	priv->can.state = CAN_STATE_ERROR_ACTIVE;
	return 0;
}

static int mcp2515_do_set_bittiming(struct net_device *net)
{
	struct mcp2515_priv *priv = netdev_priv(net);
	struct can_bittiming *bt = &priv->can.bittiming;

	mcp2515_write_reg(priv, CNF1, ((bt->sjw - 1) << CNF1_SJW_SHIFT) | (bt->brp - 1));
	mcp2515_write_reg(priv, CNF2, CNF2_BTLMODE | (priv->can.ctrlmode & CAN_CTRLMODE_3_SAMPLES ?
			   CNF2_SAM : 0) | ((bt->phase_seg1 - 1) << CNF2_PS1_SHIFT) | (bt->prop_seg - 1));
	mcp2515_write_bits(priv, CNF3, CNF3_PHSEG2_MASK, (bt->phase_seg2 - 1));
	/* dev_dbg(&spi->dev, "CNF: 0x%02x 0x%02x 0x%02x\n",
		mcp2515_read_reg(priv, CNF1),
		mcp2515_read_reg(priv, CNF2),
		mcp2515_read_reg(priv, CNF3));
	*/
	return 0;
}

static int mcp2515_setup(struct net_device *net, struct mcp2515_priv *priv) {
	mcp2515_do_set_bittiming(net);

	mcp2515_write_reg(priv, RXBCTRL(0), RXBCTRL_BUKT | RXBCTRL_RXM0 | RXBCTRL_RXM1);
	mcp2515_write_reg(priv, RXBCTRL(1), RXBCTRL_RXM0 | RXBCTRL_RXM1);
	return 0;
}

static int mcp2515_hw_reset(struct mcp2515_priv *priv) {
	u8 reg;
	int ret;

	/* Wait for oscillator startup timer after power up */
	mdelay(MCP251X_OST_DELAY_MS);

	priv->spi_tx_buf[0] = INSTRUCTION_RESET;
	ret = mcp2515_spi_trans(priv, 1);

	if (ret)
		return ret;

	/* Wait for oscillator startup timer after reset */
	mdelay(MCP251X_OST_DELAY_MS);
	
	reg = mcp2515_read_reg(priv, CANSTAT);
	if ((reg & CANCTRL_REQOP_MASK) != CANCTRL_REQOP_CONF)
		return -ENODEV;

	return 0;
}

static int mcp2515_hw_probe(struct mcp2515_priv *priv) {
	u8 ctrl;
	int ret;

	ret = mcp2515_hw_reset(priv);
	if (ret)
		return ret;

	ctrl = mcp2515_read_reg(priv, CANCTRL);

	/* dev_dbg(&spi->dev, "CANCTRL 0x%02x\n", ctrl); */

	/* Check for power up default value */
	if ((ctrl & 0x17) != 0x07)
		return -ENODEV;

	return 0;
}

static int mcp2515_power_enable(struct regulator *reg, int enable)
{
	if (IS_ERR_OR_NULL(reg))
		return 0;

	if (enable)
		return regulator_enable(reg);
	else
		return regulator_disable(reg);
}

static void mcp2515_open_clean(struct net_device *net)
{
	struct mcp2515_priv *priv = netdev_priv(net);

	free_irq(priv->irq, priv);
	mcp2515_hw_sleep(priv);
	mcp2515_power_enable(priv->transceiver, 0);
	close_candev(net);
}

static int mcp2515_stop(struct net_device *net) {
	struct mcp2515_priv *priv = netdev_priv(net);

	close_candev(net);

	priv->force_quit = 1;
	/* TODO */
	/* free_irq(spi->irq, priv); */
	destroy_workqueue(priv->wq);
	priv->wq = NULL;

	mutex_lock(&priv->mcp_lock);

	/* Disable and clear pending interrupts */
	mcp2515_write_reg(priv, CANINTE, 0x00);
	mcp2515_write_reg(priv, CANINTF, 0x00);

	mcp2515_write_reg(priv, TXBCTRL(0), 0);
	mcp2515_clean(net);

	mcp2515_power_enable(priv->transceiver, 0);

	priv->can.state = CAN_STATE_STOPPED;

	mutex_unlock(&priv->mcp_lock);

	can_led_event(net, CAN_LED_EVENT_STOP);

	return 0;
}

static void mcp2515_error_skb(struct net_device *net, int can_id, int data1)
{
	struct sk_buff *skb;
	struct can_frame *frame;

	skb = alloc_can_err_skb(net, &frame);
	if (skb) {
		frame->can_id |= can_id;
		frame->data[1] = data1;
		netif_rx_ni(skb);
	} else {
		netdev_err(net, "cannot allocate error skb\n");
	}
}

#if 0
static void mcp2515_tx_work_handler(struct work_struct *ws)
{
	struct mcp2515_priv *priv = container_of(ws, struct mcp2515_priv,
						 tx_work);
	/* struct spi_device *spi = priv->spi; */
	struct net_device *net = priv->net;
	struct can_frame *frame;

	mutex_lock(&priv->mcp_lock);
	if (priv->tx_skb) {
		if (priv->can.state == CAN_STATE_BUS_OFF) {
			mcp2515_clean(net);
		} else {
			frame = (struct can_frame *)priv->tx_skb->data;

			if (frame->can_dlc > CAN_FRAME_MAX_DATA_LEN)
				frame->can_dlc = CAN_FRAME_MAX_DATA_LEN;
			mcp2515_hw_tx(priv, frame, 0);
			priv->tx_len = 1 + frame->can_dlc;
			can_put_echo_skb(priv->tx_skb, net, 0);
			priv->tx_skb = NULL;
		}
	}
	mutex_unlock(&priv->mcp_lock);
}
#endif

#if 0

static void mcp2515_restart_work_handler(struct work_struct *ws)
{
	struct mcp2515_priv *priv = container_of(ws, struct mcp2515_priv,
						 restart_work);
	struct spi_device *spi = priv->spi;
	struct net_device *net = priv->net;

	mutex_lock(&priv->mcp_lock);
	if (priv->after_suspend) {
		mcp2515_hw_reset(priv);
		mcp2515_setup(net, priv);
		if (priv->after_suspend & AFTER_SUSPEND_RESTART) {
			mcp2515_set_normal_mode();
		} else if (priv->after_suspend & AFTER_SUSPEND_UP) {
			netif_device_attach(net);
			mcp2515_clean(net);
			mcp2515_set_normal_mode();
			netif_wake_queue(net);
		} else {
			mcp2515_hw_sleep(priv);
		}
		priv->after_suspend = 0;
		priv->force_quit = 0;
	}

	if (priv->restart_tx) {
		priv->restart_tx = 0;
		mcp2515_write_reg(priv, TXBCTRL(0), 0);
		mcp2515_clean(net);
		netif_wake_queue(net);
		mcp2515_error_skb(net, CAN_ERR_RESTARTED, 0);
	}
	mutex_unlock(&priv->mcp_lock);
}
#endif

static irqreturn_t mcp2515_can_ist(int irq, void *dev_id)
{
	struct mcp2515_priv *priv = dev_id;
	/* struct spi_device *spi = priv->spi; */
	struct net_device *net = priv->net;

	mutex_lock(&priv->mcp_lock);
	while (!priv->force_quit) {
		enum can_state new_state;
		u8 intf, eflag;
		u8 clear_intf = 0;
		int can_id = 0, data1 = 0;

		mcp2515_read_2regs(priv, CANINTF, &intf, &eflag);

		/* mask out flags we don't care about */
		intf &= CANINTF_RX | CANINTF_TX | CANINTF_ERR;

		/* receive buffer 0 */
		if (intf & CANINTF_RX0IF) {
			mcp2515_hw_rx(priv, 0);
			/*
			 * Free one buffer ASAP
			 * (The MCP2515 does this automatically.)
			 */
		}

		/* receive buffer 1 */
		if (intf & CANINTF_RX1IF) {
			mcp2515_hw_rx(priv, 1);
			/* the MCP2515 does this automatically */
		}

		/* any error or tx interrupt we need to clear? */
		if (intf & (CANINTF_ERR | CANINTF_TX))
			clear_intf |= intf & (CANINTF_ERR | CANINTF_TX);
		if (clear_intf)
			mcp2515_write_bits(priv, CANINTF, clear_intf, 0x00);

		if (eflag)
			mcp2515_write_bits(priv, EFLG, eflag, 0x00);

		/* Update can state */
		if (eflag & EFLG_TXBO) {
			new_state = CAN_STATE_BUS_OFF;
			can_id |= CAN_ERR_BUSOFF;
		} else if (eflag & EFLG_TXEP) {
			new_state = CAN_STATE_ERROR_PASSIVE;
			can_id |= CAN_ERR_CRTL;
			data1 |= CAN_ERR_CRTL_TX_PASSIVE;
		} else if (eflag & EFLG_RXEP) {
			new_state = CAN_STATE_ERROR_PASSIVE;
			can_id |= CAN_ERR_CRTL;
			data1 |= CAN_ERR_CRTL_RX_PASSIVE;
		} else if (eflag & EFLG_TXWAR) {
			new_state = CAN_STATE_ERROR_WARNING;
			can_id |= CAN_ERR_CRTL;
			data1 |= CAN_ERR_CRTL_TX_WARNING;
		} else if (eflag & EFLG_RXWAR) {
			new_state = CAN_STATE_ERROR_WARNING;
			can_id |= CAN_ERR_CRTL;
			data1 |= CAN_ERR_CRTL_RX_WARNING;
		} else {
			new_state = CAN_STATE_ERROR_ACTIVE;
		}

		/* Update can state statistics */
		switch (priv->can.state) {
		case CAN_STATE_ERROR_ACTIVE:
			if (new_state >= CAN_STATE_ERROR_WARNING &&
			    new_state <= CAN_STATE_BUS_OFF)
				priv->can.can_stats.error_warning++;
		case CAN_STATE_ERROR_WARNING:	/* fallthrough */
			if (new_state >= CAN_STATE_ERROR_PASSIVE &&
			    new_state <= CAN_STATE_BUS_OFF)
				priv->can.can_stats.error_passive++;
			break;
		default:
			break;
		}
		priv->can.state = new_state;

		if (intf & CANINTF_ERRIF) {
			/* Handle overflow counters */
			if (eflag & (EFLG_RX0OVR | EFLG_RX1OVR)) {
				if (eflag & EFLG_RX0OVR) {
					net->stats.rx_over_errors++;
					net->stats.rx_errors++;
				}
				if (eflag & EFLG_RX1OVR) {
					net->stats.rx_over_errors++;
					net->stats.rx_errors++;
				}
				can_id |= CAN_ERR_CRTL;
				data1 |= CAN_ERR_CRTL_RX_OVERFLOW;
			}
			mcp2515_error_skb(net, can_id, data1);
		}

		if (priv->can.state == CAN_STATE_BUS_OFF) {
			if (priv->can.restart_ms == 0) {
				priv->force_quit = 1;
				can_bus_off(net);
				mcp2515_hw_sleep(priv);
				break;
			}
		}

		if (intf == 0)
			break;

		if (intf & CANINTF_TX) {
			net->stats.tx_packets++;
			net->stats.tx_bytes += priv->tx_len - 1;
			can_led_event(net, CAN_LED_EVENT_TX);
			if (priv->tx_len) {
				can_get_echo_skb(net, 0);
				priv->tx_len = 0;
			}
			netif_wake_queue(net);
		}

	}
	mutex_unlock(&priv->mcp_lock);
	return IRQ_HANDLED;
}

static int mcp2515_open(struct net_device *net)
{
	struct mcp2515_priv *priv = netdev_priv(net);

	unsigned long flags = IRQF_ONESHOT | IRQF_TRIGGER_FALLING;
	int ret;

	ret = open_candev(net);
	if (ret) {
		/* TODO */
		/* dev_err(&spi->dev, "unable to set initial baudrate!\n"); */
		return ret;
	}

	mutex_lock(&priv->mcp_lock);
	mcp2515_power_enable(priv->transceiver, 1);

	priv->force_quit = 0;
	priv->tx_skb = NULL;
	priv->tx_len = 0;

	ret = request_threaded_irq(priv->irq, NULL, mcp2515_can_ist,
				   flags | IRQF_ONESHOT, DEVICE_NAME, priv);
	if (ret) {
		/* TODO */
		/* dev_err(&spi->dev, "failed to acquire irq %d\n", spi->irq); */
		mcp2515_power_enable(priv->transceiver, 0);
		close_candev(net);
		goto open_unlock;
	}

	/* TODO */
	/*
	priv->wq = create_freezable_workqueue("mcp2515_wq");
	INIT_WORK(&priv->tx_work, mcp2515_tx_work_handler);
	INIT_WORK(&priv->restart_work, mcp2515_restart_work_handler);
	*/
	ret = mcp2515_hw_reset(priv);
	if (ret) {
		mcp2515_open_clean(net);
		goto open_unlock;
	}
	ret = mcp2515_setup(net, priv);
	if (ret) {
		mcp2515_open_clean(net);
		goto open_unlock;
	}
	ret = mcp2515_set_normal_mode(net);
	if (ret) {
		mcp2515_open_clean(net);
		goto open_unlock;
	}
	/* TODO */
	/* can_led_event(net, CAN_LED_EVENT_OPEN); */

	netif_wake_queue(net);

open_unlock:
	mutex_unlock(&priv->mcp_lock);
	return ret;
}

static const struct net_device_ops mcp2515_netdev_ops = {
	.ndo_open = mcp2515_open,
	.ndo_stop = mcp2515_stop,
	.ndo_start_xmit = mcp2515_hard_start_xmit,
	.ndo_change_mtu = can_change_mtu,
};

#if 0

static const struct of_device_id mcp2515_of_match[] = {
	{
		.compatible	= "microchip,mcp2515",
		.data		= (void *)CAN_MCP251X_MCP2515,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, mcp2515_of_match);

static const struct spi_device_id mcp2515_id_table[] = {
	{
		.name		= "mcp2515",
		.driver_data	= (kernel_ulong_t)CAN_MCP251X_MCP2515,
	},
	{ }
};
MODULE_DEVICE_TABLE(spi, mcp2515_id_table);
#endif

/* static int mcp2515_can_probe(struct spi_device *spi) */
static int mcp2515_can_probe(struct platform_device *pdev)
{
	/* const struct of_device_id *of_id = of_match_device(mcp2515_of_match,
							   &spi->dev);
	struct mcp2515_platform_data *pdata = dev_get_platdata(&spi->dev); */
	struct mcp2515_priv *priv;

	struct net_device *net;
	struct clk *clk;
	int freq, ret;

	freq = 16000000;
	printk(KERN_INFO "%s: started\n", __func__);

	/* Allocate can/net device */
	net = alloc_candev(sizeof(struct mcp2515_priv), TX_ECHO_SKB_MAX);
	if (!net)
		return -ENOMEM;
	printk(KERN_INFO "%s: p1\n", __func__);

	net->netdev_ops = &mcp2515_netdev_ops;
	net->flags |= IFF_ECHO;
	printk(KERN_INFO "%s: p2\n", __func__);

	priv = netdev_priv(net);
	printk(KERN_INFO "%s: p2a\n", __func__);
	priv->can.bittiming_const = &mcp2515_bittiming_const;
	printk(KERN_INFO "%s: p2b\n", __func__);
	priv->can.do_set_mode = mcp2515_do_set_mode;
	printk(KERN_INFO "%s: p3\n", __func__);
	priv->can.clock.freq = freq / 2;
	priv->can.ctrlmode_supported = CAN_CTRLMODE_3_SAMPLES |
		CAN_CTRLMODE_LOOPBACK | CAN_CTRLMODE_LISTENONLY;
	printk(KERN_INFO "%s: p4\n", __func__);
	priv->net = net;
	printk(KERN_INFO "%s: p5\n", __func__);
	priv->clk = clk;
	printk(KERN_INFO "%s: p6\n", __func__);

	ret = mcp2515_power_enable(priv->power, 1);
	if (ret)
		goto out_clock;
	printk(KERN_INFO "%s: p7\n", __func__);

	mutex_init(&priv->mcp_lock);

	printk(KERN_INFO "%s: p8\n", __func__);

	/* GPIO stuff */
	ret = gpio_request_one(gpios[GPIO_MISO], GPIOF_IN, "MCP2515 MISO");
	printk(KERN_INFO "%s: p9\n", __func__);
        if (ret) {
		printk(KERN_ERR "can't get MISO pin GPIO%d\n", GPIO_MISO);
                goto out_clock;
	}

	ret = gpio_request_one(gpios[GPIO_MOSI], GPIOF_OUT_INIT_HIGH, "MCP2515 MOSI");
        if (ret) {
		printk(KERN_ERR "can't get MOSI pin GPIO%d\n", GPIO_MOSI);
                goto out_mosi;
	}

	ret = gpio_request_one(gpios[GPIO_CLK], GPIOF_OUT_INIT_HIGH, "MCP2515 CLK");
        if (ret) {
		printk(KERN_ERR "can't get CLK pin GPIO%d\n", GPIO_CLK);
                goto out_clk;
	}

	ret = gpio_request_one(gpios[GPIO_CS], GPIOF_OUT_INIT_HIGH, "MCP2515 CS");
        if (ret) {
		printk(KERN_ERR "can't get CS pin GPIO%d\n", GPIO_CS);
                goto out_cs;
	}

	ret = gpio_request_one(gpios[GPIO_INT], GPIOF_IN, "MCP2515 INT");
        if (ret) {
		printk(KERN_ERR "can't get INT pin GPIO%d\n", GPIO_INT);
                goto out_int;
	}
	printk(KERN_INFO "%s: p10\n", __func__);

	priv->irq=gpio_to_irq(gpios[GPIO_INT]);
	printk(KERN_INFO "%s: p11\n", __func__);
	if (priv->irq<0) {
		printk(KERN_ERR "can't map GPIO %d to IRQ : error %d\n", gpios[GPIO_INT],  priv->irq);
		goto out_int_irq;
	}

	/* SET_NETDEV_DEV(net, &spi->dev); */
	/* netif_napi_add(dev, &priv->napi, flexcan_poll, FLEXCAN_NAPI_WEIGHT); */
	/* platform_set_drvdata(pdev, dev);
	SET_NETDEV_DEV(dev, &pdev->dev); */

	/* Here is OK to not lock the MCP, no one knows about it yet */
	printk(KERN_INFO "%s: p12\n", __func__);
	ret = mcp2515_hw_probe(priv);
	printk(KERN_INFO "%s: p13\n", __func__);
	if (ret)
		goto error_probe;

	ret = register_candev(net);
	printk(KERN_INFO "%s: registered CAN device\n", __func__);
	if (ret)
		goto error_probe;

	/* devm_can_led_init(net); */

	return 0;

out_int_irq:
	gpio_free(GPIO_INT);

out_int:
	gpio_free(GPIO_CS);

out_cs:
	gpio_free(GPIO_CLK);

out_clk:
	gpio_free(GPIO_MOSI);

out_mosi:
	gpio_free(GPIO_MISO);

error_probe:
	mcp2515_power_enable(priv->power, 0);

out_clock:
	free_candev(net);

	return ret;
}

/* static int mcp2515_can_remove(struct spi_device *spi) */
static int mcp2515_can_remove(struct platform_device *pdev)
{
	struct net_device *net_dev = platform_get_drvdata(pdev);
	struct mcp2515_priv *priv = netdev_priv(net_dev);

	unregister_candev(net_dev);

	mcp2515_power_enable(priv->power, 0);

	if (!IS_ERR(priv->clk))
		clk_disable_unprepare(priv->clk);

	free_candev(net_dev);

	return 0;
}

static int __maybe_unused mcp2515_can_suspend(struct device *device)
{
	struct net_device *net_dev = dev_get_drvdata(device);
	struct mcp2515_priv *priv = netdev_priv(net_dev);

	priv->force_quit = 1;
	disable_irq(priv->irq);
	/*
	 * Note: at this point neither IST nor workqueues are running.
	 * open/stop cannot be called anyway so locking is not needed
	 */
	if (netif_running(net_dev)) {
		netif_device_detach(net_dev);

		mcp2515_hw_sleep(priv);
		mcp2515_power_enable(priv->transceiver, 0);
		priv->after_suspend = AFTER_SUSPEND_UP;
	} else {
		priv->after_suspend = AFTER_SUSPEND_DOWN;
	}

	if (!IS_ERR_OR_NULL(priv->power)) {
		regulator_disable(priv->power);
		priv->after_suspend |= AFTER_SUSPEND_POWER;
	}

	return 0;
}

static int __maybe_unused mcp2515_can_resume(struct device *device)
{
	struct net_device *net_dev = dev_get_drvdata(device);
	struct mcp2515_priv *priv = netdev_priv(net_dev);

	if (priv->after_suspend & AFTER_SUSPEND_POWER) {
		mcp2515_power_enable(priv->power, 1);
		/* queue_work(priv->wq, &priv->restart_work); */
	} else {
		if (priv->after_suspend & AFTER_SUSPEND_UP) {
			mcp2515_power_enable(priv->transceiver, 1);
			/* queue_work(priv->wq, &priv->restart_work); */
		} else {
			priv->after_suspend = 0;
		}
	}
	priv->force_quit = 0;
	enable_irq(priv->irq);
	return 0;
}

static SIMPLE_DEV_PM_OPS(mcp2515_can_pm_ops, mcp2515_can_suspend, mcp2515_can_resume);

static void mcp2515_can_device_release(struct device *dev) {
        dev_info(dev, "device release event\n");
}

static struct platform_driver mcp2515_can_driver = {
        .driver = {
                .name = DEVICE_NAME,
                .owner = THIS_MODULE,
                /* .of_match_table = mcp2515_of_match, */
                /* .pm = &mcp2515_can_pm_ops, */
        },
        /* .id_table = mcp2515_id_table, */
        .probe = mcp2515_can_probe,
        .remove = mcp2515_can_remove,
};

static struct platform_device mcp2515_can_device = {
	.name = DEVICE_NAME,
	.id   = 0,
	.dev  = {
		.release = mcp2515_can_device_release,
	},
};

/* module_platform_driver(mcp2515_can_driver); */

static int __init mcp2515_banged_init(void)
{
	int err;
        err = platform_device_register(&mcp2515_can_device);
        if (err)
                goto exit_free_device;

        err = platform_driver_register(&mcp2515_can_driver);
        if (err)
                goto exit_free_device;

        pr_info("banged MC2515 driver device registered\n");

        return 0;
exit_free_device:
	return err;
}
module_init(mcp2515_banged_init);

static void __exit mcp2515_banged_exit(void)
{
        platform_driver_unregister(&mcp2515_can_driver);
        platform_device_unregister(&mcp2515_can_device);
        pr_info("banged MCP2515 driver device unregistered\n");
}

module_exit(mcp2515_banged_exit);

MODULE_AUTHOR("Chris Elston <celston@katalix.com>, "
	      "Christian Pellegrin <chripell@evolware.org>");
MODULE_DESCRIPTION("Microchip 2515 CAN driver");
MODULE_LICENSE("GPL v2");
