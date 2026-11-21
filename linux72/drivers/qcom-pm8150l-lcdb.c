// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal Qualcomm PM8150L LCDB (+/-5.7V LCD bias) regulator driver for Meta Quest 2.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regulator/of_regulator.h>

#define LCDB_STS1_REG			0x08
#define LCDB_STS3_REG			0x0a
#define LCDB_STS4_REG			0x0b
#define LCDB_INT_RT_STATUS_REG		0x10
#define  LCDB_VREG_OK_BIT		BIT(0)

#define LCDB_BST_VOUT_REG		0x41
#define  LCDB_BST_VOUT_MASK		GENMASK(5, 0)
#define LCDB_MODULE_RDY_REG		0x45
#define  LCDB_MODULE_RDY_BIT		BIT(7)
#define LCDB_ENABLE_CTL1_REG		0x46
#define  LCDB_MODULE_EN_BIT		BIT(7)
#define LCDB_LDO_VOUT_REG		0x71
#define  LCDB_LDO_VOUT_MASK		GENMASK(4, 0)
#define LCDB_NCP_VOUT_REG		0x81
#define  LCDB_NCP_VOUT_MASK		GENMASK(4, 0)

struct pm8150l_lcdb {
	struct device *dev;
	struct regmap *regmap;
	u32 base;
	struct regulator_desc desc;
};

static int lcdb_set_mv(struct pm8150l_lcdb *lcdb, int mv)
{
	int bst_mv = clamp(mv + 150, 4700, 6275);
	u32 bst_val = DIV_ROUND_UP(bst_mv - 4700, 25);
	u32 ldo_val = mv < 4950 ? DIV_ROUND_UP(mv - 4000, 100)
				: 0x0a + DIV_ROUND_UP(mv - 4950, 50);
	int ret;

	ret = regmap_update_bits(lcdb->regmap, lcdb->base + LCDB_BST_VOUT_REG,
				 LCDB_BST_VOUT_MASK, bst_val);
	if (ret)
		return ret;

	ret = regmap_update_bits(lcdb->regmap, lcdb->base + LCDB_LDO_VOUT_REG,
				 LCDB_LDO_VOUT_MASK, ldo_val);
	if (ret)
		return ret;

	return regmap_update_bits(lcdb->regmap, lcdb->base + LCDB_NCP_VOUT_REG,
				  LCDB_NCP_VOUT_MASK, ldo_val);
}

static int lcdb_is_enabled(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	unsigned int val = 0;
	int ret;

	ret = regmap_read(lcdb->regmap, lcdb->base + LCDB_ENABLE_CTL1_REG, &val);
	if (ret)
		return ret;

	return !!(val & LCDB_MODULE_EN_BIT);
}

static int lcdb_enable(struct regulator_dev *rdev)
{
	static const int steps_mv[] = { 5000, 5500, 5700 };
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	unsigned int val = 0, sts1 = 0, sts3 = 0, sts4 = 0;
	int i, ret;

	ret = lcdb_is_enabled(rdev);
	if (ret < 0)
		return ret;
	if (ret > 0)
		return 0;

	/* Start at 4.5V (BST >= 4.7V) before step-ramping to +/-5.7V. */
	ret = lcdb_set_mv(lcdb, 4500);
	if (ret)
		return ret;

	ret = regmap_write(lcdb->regmap, lcdb->base + LCDB_ENABLE_CTL1_REG,
			   LCDB_MODULE_EN_BIT);
	if (ret)
		return ret;

	for (i = 0; i < 50; i++) {
		ret = regmap_read(lcdb->regmap, lcdb->base + LCDB_INT_RT_STATUS_REG,
				  &val);
		if (ret)
			goto err_disable;
		if (val & LCDB_VREG_OK_BIT)
			break;
		usleep_range(2000, 2200);
	}

	if (!(val & LCDB_VREG_OK_BIT)) {
		regmap_read(lcdb->regmap, lcdb->base + LCDB_STS1_REG, &sts1);
		regmap_read(lcdb->regmap, lcdb->base + LCDB_STS3_REG, &sts3);
		regmap_read(lcdb->regmap, lcdb->base + LCDB_STS4_REG, &sts4);
		dev_err(lcdb->dev,
			"LCDB vreg-ok timeout (rt=0x%02x sts1=0x%02x sts3=0x%02x sts4=0x%02x)\n",
			val, sts1, sts3, sts4);
		ret = -ETIMEDOUT;
		goto err_disable;
	}

	usleep_range(10000, 11000);

	for (i = 0; i < ARRAY_SIZE(steps_mv); i++) {
		ret = lcdb_set_mv(lcdb, steps_mv[i]);
		if (ret)
			goto err_disable;
		usleep_range(1000, 1100);
	}

	regmap_read(lcdb->regmap, lcdb->base + LCDB_STS3_REG, &sts3);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_STS4_REG, &sts4);
	dev_info(lcdb->dev,
		 "PM8150L LCDB enabled at +/-5.7V (rt=0x%02x sts3=0x%02x sts4=0x%02x)\n",
		 val, sts3, sts4);
	return 0;

err_disable:
	regmap_write(lcdb->regmap, lcdb->base + LCDB_ENABLE_CTL1_REG, 0);
	return ret;
}

static int lcdb_disable(struct regulator_dev *rdev)
{
	struct pm8150l_lcdb *lcdb = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_write(lcdb->regmap, lcdb->base + LCDB_ENABLE_CTL1_REG, 0);
	if (ret)
		return ret;

	usleep_range(20000, 21000);
	return 0;
}

static const struct regulator_ops pm8150l_lcdb_ops = {
	.enable		= lcdb_enable,
	.disable	= lcdb_disable,
	.is_enabled	= lcdb_is_enabled,
};

static int pm8150l_lcdb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = {};
	struct regulator_init_data *init_data;
	struct regulator_dev *rdev;
	struct pm8150l_lcdb *lcdb;
	unsigned int sts1 = 0, sts3 = 0, sts4 = 0, rt = 0;
	unsigned int bst = 0, rdy = 0, en = 0, ldo = 0, ncp = 0;
	int ret;

	lcdb = devm_kzalloc(dev, sizeof(*lcdb), GFP_KERNEL);
	if (!lcdb)
		return -ENOMEM;

	lcdb->dev = dev;
	lcdb->regmap = dev_get_regmap(dev->parent, NULL);
	if (!lcdb->regmap)
		return dev_err_probe(dev, -ENODEV, "failed to get parent regmap\n");

	ret = of_property_read_u32(dev->of_node, "reg", &lcdb->base);
	if (ret)
		return dev_err_probe(dev, ret, "missing reg property\n");

	regmap_read(lcdb->regmap, lcdb->base + LCDB_STS1_REG, &sts1);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_STS3_REG, &sts3);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_STS4_REG, &sts4);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_INT_RT_STATUS_REG, &rt);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_BST_VOUT_REG, &bst);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_MODULE_RDY_REG, &rdy);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_ENABLE_CTL1_REG, &en);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_LDO_VOUT_REG, &ldo);
	regmap_read(lcdb->regmap, lcdb->base + LCDB_NCP_VOUT_REG, &ncp);

	dev_info(dev,
		 "init sts1=0x%02x sts3=0x%02x sts4=0x%02x rt=0x%02x bst=0x%02x rdy=0x%02x en=0x%02x ldo=0x%02x ncp=0x%02x\n",
		 sts1, sts3, sts4, rt, bst, rdy, en, ldo, ncp);

	if (!(rdy & LCDB_MODULE_RDY_BIT)) {
		ret = regmap_update_bits(lcdb->regmap,
					 lcdb->base + LCDB_MODULE_RDY_REG,
					 LCDB_MODULE_RDY_BIT, LCDB_MODULE_RDY_BIT);
		if (ret)
			return ret;
	}

	lcdb->desc.name = "pm8150l_lcdb";
	lcdb->desc.type = REGULATOR_VOLTAGE;
	lcdb->desc.owner = THIS_MODULE;
	lcdb->desc.ops = &pm8150l_lcdb_ops;
	lcdb->desc.fixed_uV = 5700000;
	lcdb->desc.n_voltages = 1;

	init_data = of_get_regulator_init_data(dev, dev->of_node, &lcdb->desc);
	if (!init_data)
		return -EINVAL;
	init_data->constraints.apply_uV = 0;

	config.dev = dev;
	config.of_node = dev->of_node;
	config.init_data = init_data;
	config.driver_data = lcdb;
	config.regmap = lcdb->regmap;

	rdev = devm_regulator_register(dev, &lcdb->desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register pm8150l_lcdb\n");

	return 0;
}

static const struct of_device_id pm8150l_lcdb_of_match[] = {
	{ .compatible = "qcom,pm8150l-lcdb" },
	{ }
};
MODULE_DEVICE_TABLE(of, pm8150l_lcdb_of_match);

static struct platform_driver pm8150l_lcdb_driver = {
	.probe = pm8150l_lcdb_probe,
	.driver = {
		.name = "qcom-pm8150l-lcdb",
		.of_match_table = pm8150l_lcdb_of_match,
	},
};
module_platform_driver(pm8150l_lcdb_driver);

MODULE_DESCRIPTION("Qualcomm PM8150L LCDB regulator driver");
MODULE_LICENSE("GPL");
