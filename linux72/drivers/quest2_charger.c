// SPDX-License-Identifier: GPL-2.0
/*
 * Meta Quest 2 ("hollywood") battery charger: control and monitoring of the PM8150B's SMB5 charger.
 *
 * Mainline has no SMB5 driver, and Horizon's smb5 driver is gone after the kexec, so nothing manages the charger: its state is whatever Horizon left in the
 * registers. This driver does not reconfigure the charger. It
 *   - reports what the charger sees as two power supplies (a USB input and a battery charger: input voltage/current, current limit, charge state,
 *     the float voltage the charger will stop at),
 *   - keeps charging enabled (CHARGING_ENABLE_CMD) while it is safe: the battery temperature status is clean, no charger error is flagged, and the programmed fast-charge
 *     current / float voltage are inside the limits of the Quest 2 battery profile (Horizon's device tree: 4.40 V max, 1820 mA fast charge),
 *   - lets userspace inhibit charging (charge_behaviour) and change the float voltage inside 4.00..4.40 V (voltage_max): Horizon leaves it at about 4.10 V (a longevity
 *     limit), which is why a battery near 75-80 % does not charge.
 * It never touches the input current limit, the fast-charge current, JEITA or the battery profile. Register map: Meta's smb5-reg.h (Quest 2 kernel source).
 *
 * Device tree (a child of the PM8150B pmic node, SID 2):
 *	charger@1000 { compatible = "oculus,quest2-charger"; reg = <0x1000>;
 *		io-channels = <&pm8150b_adc ADC5_USB_IN_V_16>, <&pm8150b_adc ADC5_USB_IN_I>, <&pm8150b_adc ADC5_VBAT_SNS>;
 *		io-channel-names = "usbin_v", "usbin_i", "vbat"; };
 */
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>

#define CHGR_STATUS_1		0x1006		/* [2:0] charge state */
#define CHGR_STATUS_2		0x1007		/* bit1 battery over-voltage */
#define CHGR_STATUS_7		0x100d		/* battery temperature status */
#define CHARGING_ENABLE_CMD	0x1042		/* bit0 */
#define FAST_CHG_CURRENT_CFG	0x1061		/* 50 mA / step */
#define FLOAT_VOLTAGE_CFG	0x1070		/* 3.6 V + 10 mV / step */
#define POWER_PATH_STATUS	0x110b		/* bit0 valid input, bit4 USB path, bit6 USB suspended */
#define APSD_RESULT_STATUS	0x1308		/* bit0 SDP, bit1 OCP, bit2 CDP, bit3 DCP, bit4 float */
#define USBIN_PLUGIN_RT_STS	0x1310		/* bit4: USB input plugged in */
#define WDOG_PET		0x1643		/* bit0: pet the charger watchdog; unpetted, it disables charging (BITE_WDOG_DISABLE_CHARGING) */
#define USBIN_CMD_IL		0x1340		/* bit0: suspend the USB input (left set by the shutdown from Horizon) */
#define USBIN_CURRENT_LIMIT	0x1370		/* 50 mA / step */

#define STATUS_MASK		GENMASK(2, 0)
enum { ST_INHIBIT, ST_TRICKLE, ST_PRE, ST_FULLON, ST_TAPER, ST_TERMINATE, ST_PAUSE, ST_DISABLE };
#define ERR_BAT_OV		BIT(1)
#define TEMP_BAD_MASK		(BIT(3) | BIT(2) | BIT(1) | BIT(0))	/* too hot / too cold (hard limits and after-fast-charge-pause variants) */
#define VALID_INPUT		BIT(0)
#define USE_USBIN		BIT(4)
#define USBIN_SUSPENDED		BIT(6)

/* Quest 2 battery profile (Horizon device tree: qcom,max-voltage-uv = 4400000, qcom,fastchg-current-ma = 1820) */
#define FV_MIN_UV		4000000
#define FV_MAX_UV		4400000
#define FCC_MAX_UA		1900000
#define UV_PER_FV_STEP		10000
#define FV_BASE_UV		3600000
#define UA_PER_ICL_STEP		50000

struct q2chg {
	struct device *dev;
	struct regmap *rm;
	struct iio_channel *usbin_v, *usbin_i, *vbat;
	struct power_supply *usb, *bat;
	struct delayed_work work;
	struct mutex lock;
	int behaviour;			/* POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO or _INHIBIT_CHARGE */
	int edge_tries;
	unsigned int tick;
	bool unsafe;			/* programmed limits outside the battery profile: never enable charging */
};

static int rd(struct q2chg *c, unsigned int reg, unsigned int *v)
{
	return regmap_read(c->rm, reg, v);
}

static int iio_uv(struct iio_channel *ch, int *v)
{
	if (!ch)
		return -ENODEV;
	return iio_read_channel_processed(ch, v);
}

/* charge state, mapped to the power-supply class; 0 = no input */
static int q2chg_state(struct q2chg *c, unsigned int *st_raw)
{
	unsigned int s, pp;
	int ret = rd(c, CHGR_STATUS_1, &s);

	if (ret)
		return ret;
	ret = rd(c, POWER_PATH_STATUS, &pp);
	if (ret)
		return ret;
	*st_raw = s & STATUS_MASK;
	if (!(pp & VALID_INPUT))
		return POWER_SUPPLY_STATUS_DISCHARGING;
	switch (s & STATUS_MASK) {
	case ST_TRICKLE: case ST_PRE: case ST_FULLON: case ST_TAPER:
		return POWER_SUPPLY_STATUS_CHARGING;
	case ST_TERMINATE:
		return POWER_SUPPLY_STATUS_FULL;
	default:
		return POWER_SUPPLY_STATUS_NOT_CHARGING;
	}
}

/* Is it safe to enable charging? Reads only. */
static bool q2chg_safe(struct q2chg *c, const char **why)
{
	unsigned int fcc, fv, err, temp;
	int fv_uv;

	if (rd(c, FAST_CHG_CURRENT_CFG, &fcc) || rd(c, FLOAT_VOLTAGE_CFG, &fv) || rd(c, CHGR_STATUS_2, &err) || rd(c, CHGR_STATUS_7, &temp)) {
		*why = "register read failed";
		return false;
	}
	fv_uv = FV_BASE_UV + fv * UV_PER_FV_STEP;
	if (fcc * UA_PER_ICL_STEP > FCC_MAX_UA) { *why = "fast-charge current above the battery profile"; return false; }
	if (fv_uv > FV_MAX_UV) { *why = "float voltage above the battery profile"; return false; }
	if (err & ERR_BAT_OV) { *why = "battery over-voltage flagged"; return false; }
	if (temp & TEMP_BAD_MASK) { *why = "battery temperature out of range"; return false; }
	return true;
}

static void q2chg_apply(struct q2chg *c)
{
	unsigned int en, st;
	const char *why = NULL;
	bool want;

	mutex_lock(&c->lock);
	want = c->behaviour == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO && q2chg_safe(c, &why);
	if (c->behaviour == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO && !want && !c->unsafe) {
		c->unsafe = true;
		dev_warn(c->dev, "not enabling charging: %s\n", why);
	} else if (want && c->unsafe) {
		c->unsafe = false;
		dev_info(c->dev, "charging conditions are fine again\n");
	}
	/* Horizon leaves the USB input suspended across the kexec: nothing can charge or power the system from it. Un-suspend it while plugged in and safe. */
	if (want && !rd(c, USBIN_PLUGIN_RT_STS, &en) && (en & BIT(4)) && !rd(c, USBIN_CMD_IL, &en) && (en & 1)) {
		regmap_update_bits(c->rm, USBIN_CMD_IL, 1, 0);
		dev_info(c->dev, "USB input was suspended: resumed (current limit unchanged)\n");
		c->edge_tries = 0;
		regmap_update_bits(c->rm, CHARGING_ENABLE_CMD, 1, 0);	/* the charger only leaves "disabled" on a rising edge of the enable bit */
		msleep(50);
	}
	/* enable bit already set but the charger still reports "disabled" with a valid input: give it the edge (a few tries only) */
	if (want && c->edge_tries < 3 && !rd(c, POWER_PATH_STATUS, &en) && (en & VALID_INPUT) && !rd(c, CHGR_STATUS_1, &st) &&
	    (st & STATUS_MASK) == ST_DISABLE && !rd(c, CHARGING_ENABLE_CMD, &en) && (en & 1)) {
		c->edge_tries++;
		regmap_update_bits(c->rm, CHARGING_ENABLE_CMD, 1, 0);
		msleep(50);
		dev_info(c->dev, "charger idle with charging enabled: re-triggering the enable bit (%d)\n", c->edge_tries);
	}
	if (!rd(c, CHARGING_ENABLE_CMD, &en) && ((en & 1) != want)) {
		regmap_update_bits(c->rm, CHARGING_ENABLE_CMD, 1, want);
		dev_info(c->dev, "charging %s (%s)\n", want ? "enabled" : "disabled",
			 c->behaviour == POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO ? (why ? why : "auto") : "inhibited by user");
	}
	mutex_unlock(&c->lock);
}

static void q2chg_work(struct work_struct *w)
{
	struct q2chg *c = container_of(to_delayed_work(w), struct q2chg, work);

	regmap_write(c->rm, WDOG_PET, 1);
	if (!(++c->tick % 3))
		q2chg_apply(c);
	if (!(c->tick % 5))
		power_supply_changed(c->bat);
	schedule_delayed_work(&c->work, 2 * HZ);
}

/* ---- the USB input supply ---- */
static const enum power_supply_property usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX, POWER_SUPPLY_PROP_USB_TYPE,
};
#define Q2_USB_TYPES (BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) | BIT(POWER_SUPPLY_USB_TYPE_SDP) | BIT(POWER_SUPPLY_USB_TYPE_CDP) | BIT(POWER_SUPPLY_USB_TYPE_DCP))

static int usb_get(struct power_supply *psy, enum power_supply_property p, union power_supply_propval *v)
{
	struct q2chg *c = power_supply_get_drvdata(psy);
	unsigned int r;
	int uv, ret;

	switch (p) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = rd(c, POWER_PATH_STATUS, &r);
		v->intval = !ret && (r & VALID_INPUT) && (r & USE_USBIN) && !(r & USBIN_SUSPENDED);
		return ret;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:		/* the ADC channel is pre-scaled to the real VBUS (uV) */
		return iio_uv(c->usbin_v, &v->intval);
	case POWER_SUPPLY_PROP_CURRENT_NOW:		/* Meta's PM8150B buck scale: the channel is the sense voltage, 0.2 V/A, so uA = 5 * uV */
		ret = iio_uv(c->usbin_i, &uv);
		v->intval = uv * 5;
		return ret;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = rd(c, USBIN_CURRENT_LIMIT, &r);
		v->intval = r * UA_PER_ICL_STEP;
		return ret;
	case POWER_SUPPLY_PROP_USB_TYPE:
		ret = rd(c, APSD_RESULT_STATUS, &r);
		v->intval = (r & BIT(3)) ? POWER_SUPPLY_USB_TYPE_DCP : (r & BIT(2)) ? POWER_SUPPLY_USB_TYPE_CDP :
			    (r & BIT(0)) ? POWER_SUPPLY_USB_TYPE_SDP : POWER_SUPPLY_USB_TYPE_UNKNOWN;
		return ret;
	default:
		return -EINVAL;
	}
}

static const struct power_supply_desc usb_desc = {
	.name = "quest2-usb", .type = POWER_SUPPLY_TYPE_USB, .usb_types = Q2_USB_TYPES,
	.properties = usb_props, .num_properties = ARRAY_SIZE(usb_props), .get_property = usb_get,
};

/* ---- the battery charger ---- */
static const enum power_supply_property bat_props[] = {
	POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_PROP_CHARGE_TYPE, POWER_SUPPLY_PROP_VOLTAGE_NOW, POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR, POWER_SUPPLY_PROP_PRESENT,
};

static int bat_get(struct power_supply *psy, enum power_supply_property p, union power_supply_propval *v)
{
	struct q2chg *c = power_supply_get_drvdata(psy);
	unsigned int r, raw;
	int ret;

	switch (p) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = q2chg_state(c, &raw);
		if (ret < 0)
			return ret;
		v->intval = ret;
		return 0;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		ret = rd(c, CHGR_STATUS_1, &r);
		switch (r & STATUS_MASK) {
		case ST_TRICKLE: v->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE; break;
		case ST_PRE: case ST_FULLON: v->intval = POWER_SUPPLY_CHARGE_TYPE_FAST; break;
		case ST_TAPER: v->intval = POWER_SUPPLY_CHARGE_TYPE_STANDARD; break;
		default: v->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
		}
		return ret;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		return iio_uv(c->vbat, &v->intval);
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:		/* the float voltage the charger stops at */
		ret = rd(c, FLOAT_VOLTAGE_CFG, &r);
		v->intval = FV_BASE_UV + r * UV_PER_FV_STEP;
		return ret;
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX:
		ret = rd(c, FAST_CHG_CURRENT_CFG, &r);
		v->intval = r * UA_PER_ICL_STEP;
		return ret;
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		v->intval = c->behaviour;
		return 0;
	case POWER_SUPPLY_PROP_PRESENT:
		v->intval = 1;
		return 0;
	default:
		return -EINVAL;
	}
}

static int bat_set(struct power_supply *psy, enum power_supply_property p, const union power_supply_propval *v)
{
	struct q2chg *c = power_supply_get_drvdata(psy);
	int fv;

	switch (p) {
	case POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR:
		if (v->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO && v->intval != POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE)
			return -EINVAL;
		c->behaviour = v->intval;
		q2chg_apply(c);
		return 0;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		fv = v->intval;
		if (fv < FV_MIN_UV || fv > FV_MAX_UV)
			return -EINVAL;
		dev_info(c->dev, "float voltage set to %d uV\n", FV_BASE_UV + (fv - FV_BASE_UV) / UV_PER_FV_STEP * UV_PER_FV_STEP);
		return regmap_write(c->rm, FLOAT_VOLTAGE_CFG, (fv - FV_BASE_UV) / UV_PER_FV_STEP);
	default:
		return -EINVAL;
	}
}

static int bat_writeable(struct power_supply *psy, enum power_supply_property p)
{
	return p == POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR || p == POWER_SUPPLY_PROP_VOLTAGE_MAX;
}

static const struct power_supply_desc bat_desc = {
	.name = "quest2-charger", .type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = bat_props, .num_properties = ARRAY_SIZE(bat_props),
	.get_property = bat_get, .set_property = bat_set, .property_is_writeable = bat_writeable,
	.charge_behaviours = BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO) | BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE),
};

static int q2chg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config cfg = {};
	struct q2chg *c;
	unsigned int st, fv, fcc;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	c->behaviour = POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO;
	mutex_init(&c->lock);
	c->rm = dev_get_regmap(dev->parent, NULL);
	if (!c->rm)
		return dev_err_probe(dev, -ENODEV, "no PMIC regmap\n");
	c->usbin_v = devm_iio_channel_get(dev, "usbin_v");
	c->usbin_i = devm_iio_channel_get(dev, "usbin_i");
	c->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(c->usbin_v) || IS_ERR(c->usbin_i) || IS_ERR(c->vbat)) {
		if (PTR_ERR(c->usbin_v) == -EPROBE_DEFER || PTR_ERR(c->usbin_i) == -EPROBE_DEFER || PTR_ERR(c->vbat) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_warn(dev, "ADC channels unavailable: voltages and currents will not be reported\n");
		c->usbin_v = IS_ERR(c->usbin_v) ? NULL : c->usbin_v;
		c->usbin_i = IS_ERR(c->usbin_i) ? NULL : c->usbin_i;
		c->vbat = IS_ERR(c->vbat) ? NULL : c->vbat;
	}
	if (rd(c, CHGR_STATUS_1, &st) || rd(c, FLOAT_VOLTAGE_CFG, &fv) || rd(c, FAST_CHG_CURRENT_CFG, &fcc))
		return dev_err_probe(dev, -EIO, "cannot read the charger\n");
	dev_info(dev, "charger: state %u, float voltage %d uV, fast-charge %u uA (as left by Horizon)\n", st & STATUS_MASK,
		 FV_BASE_UV + fv * UV_PER_FV_STEP, (unsigned int)(fcc * UA_PER_ICL_STEP));
	platform_set_drvdata(pdev, c);

	cfg.drv_data = c;
	c->usb = devm_power_supply_register(dev, &usb_desc, &cfg);
	if (IS_ERR(c->usb))
		return PTR_ERR(c->usb);
	c->bat = devm_power_supply_register(dev, &bat_desc, &cfg);
	if (IS_ERR(c->bat))
		return PTR_ERR(c->bat);
	INIT_DELAYED_WORK(&c->work, q2chg_work);
	schedule_delayed_work(&c->work, HZ);
	return 0;
}

static void q2chg_remove(struct platform_device *pdev)
{
	struct q2chg *c = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&c->work);
}

static const struct of_device_id q2chg_of[] = { { .compatible = "oculus,quest2-charger" }, { } };
MODULE_DEVICE_TABLE(of, q2chg_of);

static struct platform_driver q2chg_driver = {
	.probe = q2chg_probe,
	.remove = q2chg_remove,
	.driver = { .name = "quest2-charger", .of_match_table = q2chg_of },
};
module_platform_driver(q2chg_driver);

MODULE_DESCRIPTION("Meta Quest 2 PM8150B/SMB5 charger monitor and safe charge control");
MODULE_LICENSE("GPL");
