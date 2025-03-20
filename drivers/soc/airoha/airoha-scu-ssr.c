// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Christian Marangi <ansuelsmth@gmail.com>
 */

#include <dt-bindings/soc/airoha,scu-ssr.h>
#include <linux/bitfield.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/airoha/airoha-scu-ssr.h>

#define AIROHA_SCU_PCIC			0x88
#define   AIROHA_SCU_PCIE_2LANE_MODE	BIT(14)

#define AIROHA_SCU_SSR3			0x94
#define   AIROHA_SCU_SSUSB_HSGMII_SEL	BIT(29)

#define AIROHA_SCU_SSTR			0x9c
#define   AIROHA_SCU_PCIE_XSI0_SEL	GENMASK(14, 13)
#define   AIROHA_SCU_PCIE_XSI0_SEL_PCIE	FIELD_PREP_CONST(AIROHA_SCU_PCIE_XSI0_SEL, 0x0)
#define   AIROHA_SCU_PCIE_XSI1_SEL	GENMASK(12, 11)
#define   AIROHA_SCU_PCIE_XSI1_SEL_PCIE	FIELD_PREP_CONST(AIROHA_SCU_PCIE_XSI0_SEL, 0x0)
#define   AIROHA_SCU_USB_PCIE_SEL	BIT(3)

#define AIROHA_SCU_MAX_SERDES_PORT	4

struct airoha_scu_ssr_priv {
	struct device *dev;
	struct regmap *regmap;

	unsigned int serdes_port[AIROHA_SCU_MAX_SERDES_PORT];
};

static const char * const airoha_scu_serdes_mode_to_str[] = {
	[AIROHA_SCU_SERDES_MODE_PCIE0_X1] = "pcie0_x1",
	[AIROHA_SCU_SERDES_MODE_PCIE0_X2] = "pcie0_x2",
	[AIROHA_SCU_SERDES_MODE_PCIE1_X1] = "pcie1_x1",
	[AIROHA_SCU_SERDES_MODE_PCIE2_X1] = "pcie2_x1",
	[AIROHA_SCU_SERDES_MODE_USB3] = "usb3",
	[AIROHA_SCU_SERDES_MODE_ETHERNET] = "ethernet",
};

static int airoha_scu_serdes_str_to_mode(const char *serdes_str)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(airoha_scu_serdes_mode_to_str); i++)
		if (!strncmp(serdes_str, airoha_scu_serdes_mode_to_str[i],
			     strlen(airoha_scu_serdes_mode_to_str[i])))
			return i;

	return -EINVAL;
}

static int airoha_scu_ssr_apply_modes(struct airoha_scu_ssr_priv *priv)
{
	int ret;

	/*
	 * This is a very bad scenario and needs to be correctly warned
	 * as it cause PCIe malfunction.
	 */
	if ((priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SERDES_MODE_PCIE0_X2 &&
	     priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] != AIROHA_SCU_SERDES_MODE_PCIE0_X2) ||
	    (priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] != AIROHA_SCU_SERDES_MODE_PCIE0_X2 &&
	     priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] == AIROHA_SCU_SERDES_MODE_PCIE0_X2)) {
		WARN(true, "Wrong Serdes configuration for PCIe0 2 Line mode. Please check DT.\n");
		return -EINVAL;
	}

	/* PCS driver takes care of setting the SCU bit for HSGMII or USXGMII */
	if (priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SERDES_MODE_PCIE0_X1 ||
	    priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SERDES_MODE_PCIE0_X2) {
		ret = regmap_update_bits(priv->regmap, AIROHA_SCU_SSTR,
					 AIROHA_SCU_PCIE_XSI0_SEL,
					 AIROHA_SCU_PCIE_XSI0_SEL_PCIE);
		if (ret)
			return ret;
	}

	/* PCS driver takes care of setting the SCU bit for HSGMII or USXGMII */
	if (priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] == AIROHA_SCU_SERDES_MODE_PCIE1_X1 ||
	    priv->serdes_port[AIROHA_SCU_SERDES_WIFI2] == AIROHA_SCU_SERDES_MODE_PCIE0_X2) {
		ret = regmap_update_bits(priv->regmap, AIROHA_SCU_SSTR,
					 AIROHA_SCU_PCIE_XSI1_SEL,
					 AIROHA_SCU_PCIE_XSI1_SEL_PCIE);
		if (ret)
			return ret;
	}

	/* Toggle PCIe0 2 Line mode if enabled or not */
	if (priv->serdes_port[AIROHA_SCU_SERDES_WIFI1] == AIROHA_SCU_SERDES_MODE_PCIE0_X2)
		ret = regmap_set_bits(priv->regmap, AIROHA_SCU_PCIC,
				      AIROHA_SCU_PCIE_2LANE_MODE);
	else
		ret = regmap_clear_bits(priv->regmap, AIROHA_SCU_PCIC,
					AIROHA_SCU_PCIE_2LANE_MODE);
	if (ret)
		return ret;

	if (priv->serdes_port[AIROHA_SCU_SERDES_USB1] == AIROHA_SCU_SERDES_MODE_ETHERNET)
		ret = regmap_clear_bits(priv->regmap, AIROHA_SCU_SSR3,
					AIROHA_SCU_SSUSB_HSGMII_SEL);
	else
		ret = regmap_set_bits(priv->regmap, AIROHA_SCU_SSR3,
				      AIROHA_SCU_SSUSB_HSGMII_SEL);
	if (ret)
		return ret;

	if (priv->serdes_port[AIROHA_SCU_SERDES_USB2] == AIROHA_SCU_SERDES_MODE_PCIE2_X1)
		ret = regmap_clear_bits(priv->regmap, AIROHA_SCU_SSTR,
					AIROHA_SCU_USB_PCIE_SEL);
	else
		ret = regmap_set_bits(priv->regmap, AIROHA_SCU_SSTR,
				      AIROHA_SCU_USB_PCIE_SEL);
	if (ret)
		return ret;

	return 0;
}

static int airoha_scu_ssr_parse_mode(struct device *dev,
				     struct airoha_scu_ssr_priv *priv,
				     const char *property_name, unsigned int port,
				     unsigned int default_mode)
{
	const struct airoha_scu_ssr_serdes_info *port_info;
	const struct airoha_scu_ssr_data *pdata;
	const char *serdes_mode;
	int mode, i;

	pdata = dev->platform_data;

	if (of_property_read_string(dev->of_node, property_name,
				    &serdes_mode)) {
		priv->serdes_port[port] = default_mode;
		return 0;
	}

	mode = airoha_scu_serdes_str_to_mode(serdes_mode);
	if (mode) {
		dev_err(dev, "invalid mode %s for %s\n", serdes_mode, property_name);
		return mode;
	}

	port_info = &pdata->ports_info[port];
	for (i = 0; i < port_info->num_modes; i++) {
		if (port_info->possible_modes[i] == mode) {
			priv->serdes_port[port] = mode;
			return 0;
		}
	}

	dev_err(dev, "mode %s not supported for %s", serdes_mode, property_name);
	return -EINVAL;
}

static int airoha_scu_ssr_probe(struct platform_device *pdev)
{
	struct airoha_scu_ssr_priv *priv;
	struct device *dev = &pdev->dev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;

	/* Get regmap from MFD */
	priv->regmap = dev_get_regmap(dev->parent, NULL);
	if (!priv->regmap)
		return -EINVAL;

	ret = airoha_scu_ssr_parse_mode(dev, priv, "airoha,serdes-wifi1",
					AIROHA_SCU_SERDES_WIFI1,
					AIROHA_SCU_SERDES_MODE_PCIE0_X1);
	if (ret)
		return ret;

	ret = airoha_scu_ssr_parse_mode(dev, priv, "airoha,serdes-wifi2",
					AIROHA_SCU_SERDES_WIFI2,
					AIROHA_SCU_SERDES_MODE_PCIE1_X1);
	if (ret)
		return ret;

	ret = airoha_scu_ssr_parse_mode(dev, priv, "airoha,serdes-usb1",
					AIROHA_SCU_SERDES_USB1,
					AIROHA_SCU_SERDES_MODE_USB3);
	if (ret)
		return ret;

	ret = airoha_scu_ssr_parse_mode(dev, priv, "airoha,serdes-usb2",
					AIROHA_SCU_SERDES_USB2,
					AIROHA_SCU_SERDES_MODE_USB3);
	if (ret)
		return ret;

	ret = airoha_scu_ssr_apply_modes(priv);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, priv);

	return 0;
}

static struct platform_driver airoha_scu_ssr_driver = {
	.probe		= airoha_scu_ssr_probe,
	.driver		= {
		.name	= "airoha-scu-ssr",
	},
};

module_platform_driver(airoha_scu_ssr_driver);

MODULE_AUTHOR("Christian Marangi <ansuelsmth@gmail.com>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Airoha SCU SSR/STR driver");
