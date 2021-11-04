// SPDX-License-Identifier: GPL-2.0
/*
 * Lantiq Tantos switch driver for ARX100 SoCs
 *
 * Copyright (C) 2011 - 2012 John Crispin <john@phrozen.org>
 * Copyright (C) 2021 - 2022 Aleksander Jan Bajkowski <olek2@wp.pl>
 */

#include <linux/clk.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <net/dsa.h>

/* Core Registers */
#define ARX100_CTLp(p)			(0x004 + 4 * (p))
#define  ARX100_CTL_DMDIO		BIT(22)
#define  ARX100_CTL_DFWD		BIT(19)
#define  ARX100_CTL_FLP			BIT(18)
#define  ARX100_CTL_FLD			BIT(17)
#define  ARX100_CTL_AD			BIT(15)
#define  ARX100_CTL_LD			BIT(14)
#define  ARX100_CTL_REDIR		BIT(13)

#define ARX100_DF_PORTMAP		0x02C
#define  ARX100_DF_PORTMAP_UP(p)	((p) << 24)
#define  ARX100_DF_PORTMAP_BP(p)	((p) << 16)
#define  ARX100_DF_PORTMAP_MP(p)	((p) << 8)
#define  ARX100_DF_PORTMAP_RP(p)	((p) << 0)

#define ARX100_GCTL0			0x068
#define  ARX100_GCTL0_SE		BIT(31)
#define  ARX100_GCTL0_MPL_MASK		GENMASK(9, 8)
#define  ARX100_GCTL0_MPL_1522		(0x0 << 8)
#define  ARX100_GCTL0_MPL_1518		(0x1 << 8)
#define  ARX100_GCTL0_MPL_1536		(0x2 << 8)

/* MDIO clock divider, clock = 25MHz/((MCS + 1) * 2) */
#define ARX100_RGMII_CTL		0x078
#define  ARX100_RGMII_CTL_MCS_SHIFT	24
#define  ARX100_RGMII_CTL_MCS_MASK	GENMASK(31, 24)
#define  ARX100_RGMII_CTL_CKIOp(p)	(0x1 << (((p) * 2) + 21))
#define  ARX100_RGMII_CTL_FRQp(p)	(0x1 << (((p) * 2) + 20))
#define  ARX100_RGMII_CTL_IS_MASKp(p)	(0x3 << (((p) * 10) + 8))
#define  ARX100_RGMII_CTL_IS_RGMIIp(p)	(0x0 << (((p) * 10) + 8))
#define  ARX100_RGMII_CTL_IS_MIIp(p)	(0x1 << (((p) * 10) + 8))
#define  ARX100_RGMII_CTL_IS_REVMIIp(p)	(0x2 << (((p) * 10) + 8))
#define  ARX100_RGMII_CTL_IS_RMIIp(p)	(0x3 << (((p) * 10) + 8))
#define  ARX100_RGMII_CTL_RXDLY_MASKp(p)	(0x3 << (((p) * 10) + 6))
#define  ARX100_RGMII_CTL_RXDLY_0p(p)		(0x0 << (((p) * 10) + 6))
#define  ARX100_RGMII_CTL_RXDLY_1_5p(p)		(0x1 << (((p) * 10) + 6))
#define  ARX100_RGMII_CTL_RXDLY_1_75p(p)	(0x2 << (((p) * 10) + 6))
#define  ARX100_RGMII_CTL_RXDLY_2p(p)		(0x3 << (((p) * 10) + 6))
#define  ARX100_RGMII_CTL_TXDLY_MASKp(p)	(0x3 << (((p) * 10) + 4))
#define  ARX100_RGMII_CTL_TXDLY_0p(p)		(0x0 << (((p) * 10) + 4))
#define  ARX100_RGMII_CTL_TXDLY_1_5p(p)		(0x1 << (((p) * 10) + 4))
#define  ARX100_RGMII_CTL_TXDLY_1_75p(p)	(0x2 << (((p) * 10) + 4))
#define  ARX100_RGMII_CTL_TXDLY_2p(p)		(0x3 << (((p) * 10) + 4))
#define  ARX100_RGMII_CTL_SPD_MASKp(p)	(0x3 << (((p) * 10) + 2))
#define  ARX100_RGMII_CTL_SPD_10p(p)	(0x0 << (((p) * 10) + 2))
#define  ARX100_RGMII_CTL_SPD_100p(p)	(0x1 << (((p) * 10) + 2))
#define  ARX100_RGMII_CTL_SPD_1000p(p)	(0x2 << (((p) * 10) + 2))
#define  ARX100_RGMII_CTL_DUPp(p)	(0x1 << (((p) * 10) + 1))
#define  ARX100_RGMII_CTL_FCEp(p)	(0x1 << (((p) * 10) + 0))

/* RMON Registers */
#define ARX100_RMON_CTL			0x000
#define  ARX100_RMON_CTL_BUSY		BIT(11)
#define  ARX100_RMON_CTL_CAC_MASK	GENMASK(10, 9)
#define  ARX100_RMON_CTL_CAC_INDIR	(0x0 << 9)
#define  ARX100_RMON_CTL_CAC_PORT_CNT	(0x1 << 9)
#define  ARX100_RMON_CTL_CAC_PORT_RST	(0x2 << 9)
#define  ARX100_RMON_CTL_CAC_ALL_RST	(0x3 << 9)
#define  ARX100_RMON_CTL_PORTCp(p)	((p) << 6)
#define  ARX100_RMON_CTL_OFFSET_MASK	GENMASK(5, 0)

#define ARX100_RMON_DATA		0x004

/* MDIO Registers */
#define ARX100_MDIO_CTL			0x000
#define  ARX100_MDIO_CTL_WR_SHIFT	16
#define  ARX100_MDIO_CTL_WR_MASK	0xffff
#define  ARX100_MDIO_CTL_MBUSY		BIT(15)
#define  ARX100_MDIO_CTL_READ		BIT(11)
#define  ARX100_MDIO_CTL_WRITE		BIT(10)
#define  ARX100_MDIO_CTL_PHYAD_SHIFT	5
#define  ARX100_MDIO_CTL_PHYAD_MASK	0x1f
#define  ARX100_MDIO_CTL_REGAD_MASK	0x1f

#define ARX100_MDIO_DATA		0x004
#define  ARX100_MDIO_DATA_RD_MASK	GENMASK(15, 0)

#define TANTOS_MAX_PACKET_LENGTH	1536

struct tantos_hw_info {
	int max_ports;
	int cpu_port;
	const struct dsa_switch_ops *ops;
};

struct tantos_priv {
	__iomem void *core;
	__iomem void *rmon;
	__iomem void *mdio;
	const struct tantos_hw_info *hw_info;
	struct dsa_switch *ds;
	struct device *dev;
};

struct tantos_rmon_cnt_desc {
	unsigned int size;
	unsigned int offset;
	const char *name;
};

#define MIB_DESC(_size, _offset, _name) {.size = _size, .offset = _offset, .name = _name}

static const struct tantos_rmon_cnt_desc tantos_rmon_cnt[] = {
	/** Receive Packet Count (only packets that are accepted and not discarded). */
	MIB_DESC(1, 0x00, "RxUnicastPkts"),
	MIB_DESC(1, 0x01, "RxBroadcastPkts"),
	MIB_DESC(1, 0x02, "RxMulticastPkts"),
	MIB_DESC(1, 0x03, "RxFCSErrorPkts"),
	MIB_DESC(1, 0x04, "RxUnderSizeGoodPkts"),
	MIB_DESC(1, 0x05, "RxOversizeGoodPkts"),
	MIB_DESC(1, 0x06, "RxUnderSizeErrorPkts"),
	MIB_DESC(1, 0x07, "RxGoodPausePkts"),
	MIB_DESC(1, 0x08, "RxOversizeErrorPkts"),
	MIB_DESC(1, 0x09, "RxAlignErrorPkts"),
	MIB_DESC(1, 0x0A, "FilteredPkts"),
	MIB_DESC(1, 0x0B, "Rx64BytePkts"),
	MIB_DESC(1, 0x0C, "Rx127BytePkts"),
	MIB_DESC(1, 0x0D, "Rx255BytePkts"),
	MIB_DESC(1, 0x0E, "Rx511BytePkts"),
	MIB_DESC(1, 0x0F, "Rx1023BytePkts"),
	/** Receive Size 1024-1522 (or more, if configured) Packet Count. */
	MIB_DESC(1, 0x10, "RxMaxBytePkts"),
	MIB_DESC(1, 0x11, "TxUnicastPkts"),
	MIB_DESC(1, 0x12, "TxBroadcastPkts"),
	MIB_DESC(1, 0x13, "TxMulticastPkts"),
	MIB_DESC(1, 0x14, "TxSingleCollCount"),
	MIB_DESC(1, 0x15, "TxMultCollCount"),
	MIB_DESC(1, 0x16, "TxLateCollCount"),
	MIB_DESC(1, 0x17, "TxExcessCollCount"),
	MIB_DESC(1, 0x18, "TxCollCount"),
	MIB_DESC(1, 0x19, "TxPauseCount"),
	MIB_DESC(1, 0x1A, "Tx64BytePkts"),
	MIB_DESC(1, 0x1B, "Tx127BytePkts"),
	MIB_DESC(1, 0x1C, "Tx255BytePkts"),
	MIB_DESC(1, 0x1D, "Tx511BytePkts"),
	MIB_DESC(1, 0x1E, "Tx1023BytePkts"),
	/** Transmit Size 1024-1522 (or more, if configured) Packet Count. */
	MIB_DESC(1, 0x1F, "TxMaxBytePkts"),
	MIB_DESC(1, 0x20, "TxDroppedPkts"),
	MIB_DESC(1, 0x21, "RxDroppedPkts"),
	MIB_DESC(2, 0x23, "RxGoodBytes"),
	MIB_DESC(2, 0x25, "RxBadBytes"),
	MIB_DESC(2, 0x27, "TxGoodBytes"),
};

static u32 tantos_switch_r(struct tantos_priv *priv, u32 offset)
{
	return __raw_readl(priv->core + offset);
}

static void tantos_switch_w(struct tantos_priv *priv, u32 val, u32 offset)
{
	__raw_writel(val, priv->core + offset);
}

static void tantos_switch_mask(struct tantos_priv *priv, u32 clear, u32 set,
			       u32 offset)
{
	u32 val = tantos_switch_r(priv, offset);

	val &= ~(clear);
	val |= set;
	tantos_switch_w(priv, val, offset);
}

static u32 tantos_rmon_r(struct tantos_priv *priv, u32 offset)
{
	return __raw_readl(priv->rmon + offset);
}

static void tantos_rmon_w(struct tantos_priv *priv, u32 val, u32 offset)
{
	__raw_writel(val, priv->rmon + offset);
}

static u32 tantos_rmon_r_timeout(struct tantos_priv *priv, u32 offset, u32 cleared)
{
	u32 val;

	return readx_poll_timeout(__raw_readl, priv->rmon + offset, val,
				  (val & cleared) == 0, 20, 50000);
}

static u32 tantos_mdio_r(struct tantos_priv *priv, u32 offset)
{
	return __raw_readl(priv->mdio + offset);
}

static void tantos_mdio_w(struct tantos_priv *priv, u32 val, u32 offset)
{
	__raw_writel(val, priv->mdio + offset);
}

static int tantos_mdio_poll(struct tantos_priv *priv)
{
	int cnt = 100;

	while (likely(cnt--)) {
		u32 ctrl = tantos_mdio_r(priv, ARX100_MDIO_CTL);

		if ((ctrl & ARX100_MDIO_CTL_MBUSY) == 0)
			return 0;
		usleep_range(20, 40);
	}

	return -ETIMEDOUT;
}

static int tantos_mdio_wr(struct mii_bus *bus, int addr, int reg, u16 val)
{
	struct tantos_priv *priv = bus->priv;
	int err;

	err = tantos_mdio_poll(priv);
	if (err) {
		dev_err(&bus->dev, "waiting for MDIO bus busy timed out\n");
		return err;
	}

	tantos_mdio_w(priv, ARX100_MDIO_CTL_MBUSY | ARX100_MDIO_CTL_WRITE |
		((val & ARX100_MDIO_CTL_WR_MASK) << ARX100_MDIO_CTL_WR_SHIFT) |
		((addr & ARX100_MDIO_CTL_PHYAD_MASK) << ARX100_MDIO_CTL_PHYAD_SHIFT) |
		(reg & ARX100_MDIO_CTL_REGAD_MASK), ARX100_MDIO_CTL);

	return 0;
}

static int tantos_mdio_rd(struct mii_bus *bus, int addr, int reg)
{
	struct tantos_priv *priv = bus->priv;
	int err;

	err = tantos_mdio_poll(priv);
	if (err) {
		dev_err(&bus->dev, "waiting for MDIO bus busy timed out\n");
		return err;
	}

	tantos_mdio_w(priv, ARX100_MDIO_CTL_MBUSY | ARX100_MDIO_CTL_READ |
		((addr & ARX100_MDIO_CTL_PHYAD_MASK) << ARX100_MDIO_CTL_PHYAD_SHIFT) |
		(reg & ARX100_MDIO_CTL_REGAD_MASK), ARX100_MDIO_CTL);

	err = tantos_mdio_poll(priv);
	if (err) {
		dev_err(&bus->dev, "waiting for MDIO bus busy timed out\n");
		return err;
	}

	return tantos_mdio_r(priv, ARX100_MDIO_DATA) & ARX100_MDIO_DATA_RD_MASK;
}

static int tantos_mdio(struct tantos_priv *priv)
{
	struct device_node *mdio_np, *switch_np = priv->dev->of_node;
	struct device *dev = priv->dev;
	struct mii_bus *bus;
	int err = 0;

	mdio_np = of_get_compatible_child(switch_np, "lantiq,arx100-mdio");
	if (!of_device_is_available(mdio_np))
		goto out_put_node;

	bus = devm_mdiobus_alloc(dev);
	if (!bus) {
		err = -ENOMEM;
		goto out_put_node;
	}

	bus->priv = priv;
	bus->read = tantos_mdio_rd;
	bus->write = tantos_mdio_wr;
	bus->name = "lantiq,arx100-mdio";
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s-mii", dev_name(priv->dev));
	bus->parent = priv->dev;

	err = devm_of_mdiobus_register(dev, bus, mdio_np);

out_put_node:
	of_node_put(mdio_np);

	return err;
}

static int tantos_setup(struct dsa_switch *ds)
{
	struct tantos_priv *priv = ds->priv;
	unsigned int cpu_port = priv->hw_info->cpu_port;
	int i;

	/* Enable switch */
	tantos_switch_mask(priv, 0, ARX100_GCTL0_SE, ARX100_GCTL0);

	/* Enable direct forwarding */
	for (i = 0; i < priv->hw_info->max_ports; i++)
		tantos_switch_mask(priv, 0, ARX100_CTL_DFWD, ARX100_CTLp(i));

	/* Default unknown Broadcast/Multicast/Unicast port maps */
	tantos_switch_w(priv, ARX100_DF_PORTMAP_UP(BIT(cpu_port)) |
			ARX100_DF_PORTMAP_BP(BIT(cpu_port)) |
			ARX100_DF_PORTMAP_MP(BIT(cpu_port)), ARX100_DF_PORTMAP);

	/* Deactivate MDIO PHY auto polling. Some PHYs as the AR8030 have an
	 * interoperability problem with this auto polling mechanism because
	 * their status registers think that the link is in a different state
	 * than it actually is. For the AR8030 it has the BMSR_ESTATEN bit set
	 * as well as ESTATUS_1000_TFULL and ESTATUS_1000_XFULL. This makes the
	 * auto polling state machine consider the link being negotiated with
	 * 1Gbit/s. Since the PHY itself is a Fast Ethernet RMII PHY this leads
	 * to the switch port being completely dead (RX and TX are both not
	 * working).
	 * Also with various other PHY / port combinations (PHY11G GPHY, PHY22F
	 * GPHY, external RGMII PEF7071/7072) any traffic would stop. Sometimes
	 * it would work fine for a few minutes to hours and then stop, on
	 * other device it would no traffic could be sent or received at all.
	 * Testing shows that when PHY auto polling is disabled these problems
	 * go away.
	 */
	for (i = 0; i < priv->hw_info->max_ports; i++)
		tantos_switch_mask(priv, 0, ARX100_CTL_DMDIO, ARX100_CTLp(i));

	/* Configure the MDIO Clock 2.5 MHz */
	tantos_switch_mask(priv, ARX100_RGMII_CTL_MCS_MASK,
			(4 << ARX100_RGMII_CTL_MCS_SHIFT), ARX100_RGMII_CTL);

	ds->mtu_enforcement_ingress = true;

	return 0;
}

static int tantos_port_enable(struct dsa_switch *ds, int port,
			      struct phy_device *phydev)
{
	struct tantos_priv *priv = ds->priv;

	tantos_switch_mask(priv, ARX100_CTL_FLD, ARX100_CTL_FLP, ARX100_CTLp(port));

	return 0;
}

static void tantos_port_disable(struct dsa_switch *ds, int port)
{
	struct tantos_priv *priv = ds->priv;

	tantos_switch_mask(priv, ARX100_CTL_FLP, ARX100_CTL_FLD, ARX100_CTLp(port));
}

static enum dsa_tag_protocol tantos_get_tag_protocol(struct dsa_switch *ds,
						     int port,
						     enum dsa_tag_protocol mp)
{
	return DSA_TAG_PROTO_TANTOS;
}

static void tantos_port_set_speed(struct tantos_priv *priv, int port, int speed,
				  phy_interface_t interface)
{
	u32 rgmii_ctl;

	switch (speed) {
	case SPEED_10:
		rgmii_ctl = ARX100_RGMII_CTL_SPD_10p(port);
		break;
	case SPEED_100:
		rgmii_ctl = ARX100_RGMII_CTL_SPD_100p(port);
		break;
	case SPEED_1000:
		rgmii_ctl = ARX100_RGMII_CTL_SPD_1000p(port);
		break;
	}

	tantos_switch_mask(priv, ARX100_RGMII_CTL_SPD_MASKp(port), rgmii_ctl, ARX100_RGMII_CTL);
}

static void tantos_port_set_duplex(struct tantos_priv *priv, int port, int duplex)
{
	if (duplex == DUPLEX_FULL)
		tantos_switch_mask(priv, 0, ARX100_RGMII_CTL_DUPp(port), ARX100_RGMII_CTL);
	else
		tantos_switch_mask(priv, ARX100_RGMII_CTL_DUPp(port), 0, ARX100_RGMII_CTL);
}

static void tantos_port_set_pause(struct tantos_priv *priv, int port,
				  bool tx_pause, bool rx_pause)
{
	if (tx_pause || rx_pause)
		tantos_switch_mask(priv, 0, ARX100_RGMII_CTL_FCEp(port), ARX100_RGMII_CTL);
	else
		tantos_switch_mask(priv, ARX100_RGMII_CTL_FCEp(port), 0, ARX100_RGMII_CTL);
}

static int tantos_port_max_mtu(struct dsa_switch *ds, int port)
{
	/* Includes 8 bytes for special header. */
	return TANTOS_MAX_PACKET_LENGTH - VLAN_ETH_HLEN - ETH_FCS_LEN;
}

static int tantos_port_change_mtu(struct dsa_switch *ds, int port, int new_mtu)
{
	struct tantos_priv *priv = ds->priv;
	int cpu_port = priv->hw_info->cpu_port;
	u32 mpl;

	/* CPU port always has maximum mtu of user ports, so use it to set
	 * switch frame size.
	 */
	if (port == cpu_port) {
		new_mtu += (VLAN_ETH_HLEN + ETH_FCS_LEN);
		if (new_mtu <= 1518)
			mpl = ARX100_GCTL0_MPL_1518;
		else if ((new_mtu > 1518) && (new_mtu <= 1522))
			mpl = ARX100_GCTL0_MPL_1522;
		else
			mpl = ARX100_GCTL0_MPL_1536;

		tantos_switch_mask(priv, ARX100_GCTL0_MPL_MASK, mpl, ARX100_GCTL0);
	}

	return 0;
}

static void tantos_phylink_get_caps(struct dsa_switch *ds, int port,
				    struct phylink_config *config)
{
	switch (port) {
	case 0:
	case 1:
		phy_interface_set_rgmii(config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_MII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_REVMII,
			  config->supported_interfaces);
		__set_bit(PHY_INTERFACE_MODE_RMII,
			  config->supported_interfaces);
		break;
	}

	config->mac_capabilities = MAC_ASYM_PAUSE | MAC_SYM_PAUSE |
		MAC_10 | MAC_100 | MAC_1000;
}

static void tantos_phylink_mac_config(struct phylink_config *config,
				      unsigned int mode,
				      const struct phylink_link_state *state)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct tantos_priv *priv = dp->ds->priv;
	int port = dp->index;
	u32 ctl = 0;

	switch (state->interface) {
	case PHY_INTERFACE_MODE_MII:
		ctl |= ARX100_RGMII_CTL_IS_MIIp(port);
		break;
	case PHY_INTERFACE_MODE_REVMII:
		ctl |= ARX100_RGMII_CTL_IS_REVMIIp(port);
		break;
	case PHY_INTERFACE_MODE_RMII:
		ctl |= ARX100_RGMII_CTL_IS_RMIIp(port);
		break;
	case PHY_INTERFACE_MODE_RGMII:
	case PHY_INTERFACE_MODE_RGMII_ID:
	case PHY_INTERFACE_MODE_RGMII_RXID:
	case PHY_INTERFACE_MODE_RGMII_TXID:
		ctl |= ARX100_RGMII_CTL_IS_RGMIIp(port);
		break;
	default:
		dev_err(dp->ds->dev,
			"Unsupported interface: %d\n", state->interface);
		return;
	}

	tantos_switch_mask(priv, ARX100_RGMII_CTL_IS_MASKp(port), ctl, ARX100_RGMII_CTL);

	switch (state->interface) {
	case PHY_INTERFACE_MODE_RGMII:
		tantos_switch_mask(priv, ARX100_RGMII_CTL_TXDLY_MASKp(port) |
				   ARX100_RGMII_CTL_RXDLY_MASKp(port), 0, ARX100_RGMII_CTL);
		break;
	case PHY_INTERFACE_MODE_RGMII_ID:
		tantos_switch_mask(priv, ARX100_RGMII_CTL_TXDLY_MASKp(port) |
				   ARX100_RGMII_CTL_RXDLY_MASKp(port),
				   ARX100_RGMII_CTL_TXDLY_1_75p(port) |
				   ARX100_RGMII_CTL_RXDLY_1_75p(port), ARX100_RGMII_CTL);
		break;
	case PHY_INTERFACE_MODE_RGMII_RXID:
		tantos_switch_mask(priv, ARX100_RGMII_CTL_RXDLY_MASKp(port),
				   ARX100_RGMII_CTL_RXDLY_1_75p(port), ARX100_RGMII_CTL);
		break;
	case PHY_INTERFACE_MODE_RGMII_TXID:
		tantos_switch_mask(priv, ARX100_RGMII_CTL_TXDLY_MASKp(port),
				   ARX100_RGMII_CTL_TXDLY_1_75p(port), ARX100_RGMII_CTL);
		break;
	default:
		break;
	}
}

static void tantos_phylink_mac_link_down(struct phylink_config *config,
					 unsigned int mode,
					 phy_interface_t interface)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	int port = dp->index;

	if (!dsa_port_is_cpu(dp))
		tantos_port_disable(dp->ds, port);
}

static void tantos_phylink_mac_link_up(struct phylink_config *config,
				       struct phy_device *phydev,
				       unsigned int mode,
				       phy_interface_t interface,
				       int speed, int duplex,
				       bool tx_pause, bool rx_pause)
{
	struct dsa_port *dp = dsa_phylink_to_port(config);
	struct tantos_priv *priv = dp->ds->priv;
	int port = dp->index;

	if (!dsa_port_is_cpu(dp)) {
		tantos_port_set_speed(priv, port, speed, interface);
		tantos_port_set_duplex(priv, port, duplex);
		tantos_port_set_pause(priv, port, tx_pause, rx_pause);
		tantos_port_enable(dp->ds, port, NULL);
	}
}

static void tantos_get_strings(struct dsa_switch *ds, int port, u32 stringset,
			       uint8_t *data)
{
	int i;

	if (stringset != ETH_SS_STATS)
		return;

	for (i = 0; i < ARRAY_SIZE(tantos_rmon_cnt); i++)
		strncpy(data + i * ETH_GSTRING_LEN, tantos_rmon_cnt[i].name,
			ETH_GSTRING_LEN);
}

static u32 tantos_rmon_entry_read(struct tantos_priv *priv, u32 table,
				  u32 index)
{
	u32 result = 0;

	if (tantos_rmon_r_timeout(priv, ARX100_RMON_CTL, ARX100_RMON_CTL_BUSY))
		return result;

	tantos_rmon_w(priv, (ARX100_RMON_CTL_BUSY | ARX100_RMON_CTL_PORTCp(table) |
			     index), ARX100_RMON_CTL);

	if (tantos_rmon_r_timeout(priv, ARX100_RMON_CTL, ARX100_RMON_CTL_BUSY))
		return result;

	result = tantos_rmon_r(priv, ARX100_RMON_DATA);

	return result;
}

static void tantos_get_ethtool_stats(struct dsa_switch *ds, int port,
				     uint64_t *data)
{
	struct tantos_priv *priv = ds->priv;
	const struct tantos_rmon_cnt_desc *rmon_cnt;
	u64 high;
	int i;

	for (i = 0; i < ARRAY_SIZE(tantos_rmon_cnt); i++) {
		rmon_cnt = &tantos_rmon_cnt[i];

		data[i] = tantos_rmon_entry_read(priv, port, rmon_cnt->offset);

		if (rmon_cnt->size == 2) {
			high = tantos_rmon_entry_read(priv, port,
						      rmon_cnt->offset - 1);
			data[i] |= high << 32;
		}
	}
}

static int tantos_get_sset_count(struct dsa_switch *ds, int port, int sset)
{
	if (sset != ETH_SS_STATS)
		return 0;

	return ARRAY_SIZE(tantos_rmon_cnt);
}

static const struct phylink_mac_ops tantos_phylink_mac_ops = {
	.mac_config		= tantos_phylink_mac_config,
	.mac_link_down		= tantos_phylink_mac_link_down,
	.mac_link_up		= tantos_phylink_mac_link_up,
};

static const struct dsa_switch_ops tantos_arx100_switch_ops = {
	.get_tag_protocol	= tantos_get_tag_protocol,
	.setup			= tantos_setup,
	.port_enable		= tantos_port_enable,
	.port_disable		= tantos_port_disable,
	.port_change_mtu	= tantos_port_change_mtu,
	.port_max_mtu		= tantos_port_max_mtu,
	.phylink_get_caps	= tantos_phylink_get_caps,
	.get_strings		= tantos_get_strings,
	.get_ethtool_stats	= tantos_get_ethtool_stats,
	.get_sset_count		= tantos_get_sset_count,
};

static int tantos_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tantos_priv *priv;
	struct device_node *np;
	int err;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->core = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->core))
		return PTR_ERR(priv->core);

	priv->rmon = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(priv->rmon))
		return PTR_ERR(priv->rmon);

	priv->mdio = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(priv->mdio))
		return PTR_ERR(priv->mdio);

	priv->hw_info = of_device_get_match_data(dev);
	if (!priv->hw_info)
		return -EINVAL;

	priv->ds = devm_kzalloc(dev, sizeof(*priv->ds), GFP_KERNEL);
	if (!priv->ds)
		return -ENOMEM;

	priv->ds->dev = dev;
	priv->ds->num_ports = priv->hw_info->max_ports;
	priv->ds->priv = priv;
	priv->ds->ops = priv->hw_info->ops;
	priv->ds->phylink_mac_ops = &tantos_phylink_mac_ops;
	priv->dev = dev;

	np = dev->of_node;

	/* Bring up the mdio bus */
	err = tantos_mdio(priv);
	if (err) {
		dev_err(dev, "mdio probe failed\n");
		return (err);
	}

	err = dsa_register_switch(priv->ds);
	if (err) {
		dev_err(dev, "dsa switch register failed: %i\n", err);
		return (err);
	}
	if (!dsa_is_cpu_port(priv->ds, priv->hw_info->cpu_port)) {
		dev_err(dev, "wrong CPU port defined, HW only supports port: %i",
			priv->hw_info->cpu_port);
		err = -EINVAL;
		goto disable_switch;
	}

	platform_set_drvdata(pdev, priv);

	dev_info(dev, "probed embedded Tantos");
	return 0;

disable_switch:
	tantos_switch_mask(priv, ARX100_GCTL0_SE, 0, ARX100_GCTL0);
	dsa_unregister_switch(priv->ds);

	return err;
}

static void tantos_remove(struct platform_device *pdev)
{
	struct tantos_priv *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	/* Disable the switch */
	tantos_switch_mask(priv, ARX100_GCTL0_SE, 0, ARX100_GCTL0);

	dsa_unregister_switch(priv->ds);
}

static void tantos_shutdown(struct platform_device *pdev)
{
	struct tantos_priv *priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	dsa_switch_shutdown(priv->ds);

	platform_set_drvdata(pdev, NULL);
}

static const struct tantos_hw_info tantos_arx100 = {
	.max_ports = 3,
	.cpu_port = 2,
	.ops = &tantos_arx100_switch_ops,
};

static const struct of_device_id tantos_of_match[] = {
	{ .compatible = "lantiq,arx100-tantos", .data = &tantos_arx100 },
	{},
};
MODULE_DEVICE_TABLE(of, tantos_of_match);

static struct platform_driver tantos_driver = {
	.probe = tantos_probe,
	.remove = tantos_remove,
	.shutdown = tantos_shutdown,
	.driver = {
		.name = "tantos",
		.of_match_table = tantos_of_match,
	},
};

module_platform_driver(tantos_driver);

MODULE_AUTHOR("Aleksander Jan Bajkowski <olek2@wp.pl>");
MODULE_DESCRIPTION("Lantiq Tantos switch driver");
MODULE_LICENSE("GPL");
