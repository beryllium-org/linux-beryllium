// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 Rockchip Electronics Co., Ltd.
 * Copyright (c) 2024 Fyde Innovations
 *
 * Author: Wang Jie <dave.wang@rock-chips.com>
 *
 * Hynetek HUSB311 USB Type-C Port Controller Driver.
 *
 * The HUSB311 is a TCPCI-compliant PD controller with a few vendor
 * registers (0x80..0xff) on top of the standard TCPC map.  This is a
 * port of the Rockchip vendor kernel driver to mainline conventions,
 * modelled after tcpci_rt1711h.c and tcpci_mt6370.c.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/tcpci.h>
#include <linux/usb/tcpm.h>
#include <linux/workqueue.h>

#define HUSB311_VID			0x2e99
#define HUSB311_PID			0x0311

/* Vendor registers (outside the standard TCPC map) */
#define HUSB311_POWER_CTRL		0x90
#define HUSB311_POWER_CTRL_OSC_EN	BIT(0)
#define HUSB311_SOFT_RESET		0xa0
#define HUSB311_SOFT_RESET_TRIGGER	0x01
#define HUSB311_TCPC_FILTER		0xa1
#define HUSB311_TDRP			0xa2
#define HUSB311_DC_SRC_DRP		0xa3

/*
 * tTCPCfilter : (26.7 * val) us.  The same value is used as a marker in
 * husb311_resume() to detect whether the chip lost power during suspend.
 */
#define HUSB311_FILTER_DEFAULT		0x0f
/* tDRP : (51.2 + 6.4 * val) ms */
#define HUSB311_TDRP_DEFAULT		0x04
/* dcSRC.DRP : 33% duty cycle for DRP toggling */
#define HUSB311_DC_SRC_DRP_DEFAULT	330

/* Delay before the idle (24MHz oscillator off) work runs */
#define HUSB311_OSC_OFF_DELAY		(3 * HZ)

struct husb311_chip {
	struct tcpci_data data;
	struct tcpci *tcpci;
	struct device *dev;
	struct regulator *vbus;
	/* Locks vbus_on / sink_on / osc_off against the IRQ and PM paths */
	struct mutex lock;
	struct delayed_work osc_off_work;
	bool vbus_on;
	bool sink_on;
	bool osc_off;
	bool wakeup;
};

static struct husb311_chip *tdata_to_husb311(struct tcpci_data *tdata)
{
	return container_of(tdata, struct husb311_chip, data);
}

static int husb311_read8(struct husb311_chip *chip, unsigned int reg, u8 *val)
{
	return regmap_raw_read(chip->data.regmap, reg, val, sizeof(u8));
}

static int husb311_write8(struct husb311_chip *chip, unsigned int reg, u8 val)
{
	return regmap_raw_write(chip->data.regmap, reg, &val, sizeof(u8));
}

static int husb311_write16(struct husb311_chip *chip, unsigned int reg, u16 val)
{
	return regmap_raw_write(chip->data.regmap, reg, &val, sizeof(u16));
}

static const struct regmap_config husb311_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0xff, /* 0x80..0xff are vendor defined */
};

static int husb311_osc_enable(struct husb311_chip *chip, bool enable)
{
	/*
	 * The 24MHz oscillator powers the chip's timing base.  It can be
	 * disabled to save power; per the vendor the chip re-enables it
	 * automatically when an interrupt occurs.  i2c accesses while it
	 * is off still work, which the vendor's suspend path relies on.
	 */
	return regmap_update_bits(chip->data.regmap, HUSB311_POWER_CTRL,
				  HUSB311_POWER_CTRL_OSC_EN,
				  enable ? HUSB311_POWER_CTRL_OSC_EN : 0);
}

static void husb311_osc_off_work(struct work_struct *work)
{
	struct husb311_chip *chip =
		container_of(work, struct husb311_chip, osc_off_work.work);
	int ret;

	mutex_lock(&chip->lock);

	/* Keep the oscillator running while VBUS is on or we are sinking */
	if (chip->vbus_on || chip->sink_on || chip->osc_off) {
		mutex_unlock(&chip->lock);
		return;
	}

	ret = husb311_osc_enable(chip, false);
	if (ret)
		dev_err(chip->dev, "failed to disable 24MHz oscillator: %pe\n",
			ERR_PTR(ret));
	else
		chip->osc_off = true;

	mutex_unlock(&chip->lock);
}

static void husb311_osc_kick(struct husb311_chip *chip)
{
	/*
	 * Called with chip->lock held.  The chip re-enables its oscillator
	 * on any interrupt, so only clear our bookkeeping; nothing to do on
	 * the wire if the hardware already did.
	 */
	chip->osc_off = false;
}

static int husb311_sw_reset(struct husb311_chip *chip)
{
	int ret;

	ret = husb311_write8(chip, HUSB311_SOFT_RESET,
			     HUSB311_SOFT_RESET_TRIGGER);
	if (ret)
		return ret;

	usleep_range(1000, 2000);
	return 0;
}

static int husb311_init(struct tcpci *tcpci, struct tcpci_data *tdata)
{
	struct husb311_chip *chip = tdata_to_husb311(tdata);
	int ret;

	ret = husb311_write8(chip, HUSB311_TCPC_FILTER,
			     HUSB311_FILTER_DEFAULT);
	if (ret)
		return ret;

	ret = husb311_write8(chip, HUSB311_TDRP, HUSB311_TDRP_DEFAULT);
	if (ret)
		return ret;

	return husb311_write16(chip, HUSB311_DC_SRC_DRP,
			       HUSB311_DC_SRC_DRP_DEFAULT);
}

static int husb311_set_vbus(struct tcpci *tcpci, struct tcpci_data *tdata,
			    bool source, bool sink)
{
	struct husb311_chip *chip = tdata_to_husb311(tdata);
	int ret = 0;

	mutex_lock(&chip->lock);

	if (chip->vbus_on != source) {
		if (source)
			ret = regulator_enable(chip->vbus);
		else
			ret = regulator_disable(chip->vbus);
		if (ret) {
			dev_err(chip->dev, "failed to %s vbus regulator: %pe\n",
				source ? "enable" : "disable", ERR_PTR(ret));
			goto out;
		}
		chip->vbus_on = source;
	}
	chip->sink_on = sink;

	/* Interrupts have woken the oscillator back up if it was off */
	husb311_osc_kick(chip);

	/*
	 * Re-check a bit later: if we are neither sourcing nor sinking,
	 * switch the oscillator off to save power.
	 */
	queue_delayed_work(system_freezable_wq, &chip->osc_off_work,
			   HUSB311_OSC_OFF_DELAY);

out:
	mutex_unlock(&chip->lock);
	return ret;
}

static irqreturn_t husb311_irq(int irq, void *dev_id)
{
	struct husb311_chip *chip = dev_id;
	enum typec_cc_status cc1, cc2;
	u8 status = 0, role_ctrl, cc_status;

	/*
	 * Vendor workaround for non-standard Huawei-type cables that
	 * present Rp on both CC lines: when one CC reads Rp, force the
	 * other CC to Open so it is not detected as a second source.
	 * This runs before the TCPCI core handles the interrupt.
	 * XXX: needs hardware verification; without a doubled-Rp cable
	 * there is no easy way to tell whether this is still required.
	 */
	if (!husb311_read8(chip, TCPC_ALERT, &status) &&
	    status & TCPC_ALERT_CC_STATUS) {
		if (!husb311_read8(chip, TCPC_ROLE_CTRL, &role_ctrl) &&
		    !husb311_read8(chip, TCPC_CC_STATUS, &cc_status)) {
			cc1 = tcpci_to_typec_cc(FIELD_GET(TCPC_CC_STATUS_CC1,
							  cc_status),
						cc_status & TCPC_CC_STATUS_TERM ||
						tcpc_presenting_rd(role_ctrl,
								   CC1));
			cc2 = tcpci_to_typec_cc(FIELD_GET(TCPC_CC_STATUS_CC2,
							  cc_status),
						cc_status & TCPC_CC_STATUS_TERM ||
						tcpc_presenting_rd(role_ctrl,
								   CC2));

			if (cc1 == TYPEC_CC_RP_DEF)
				role_ctrl = FIELD_PREP(TCPC_ROLE_CTRL_CC2,
						       TCPC_ROLE_CTRL_CC_OPEN) |
					    (role_ctrl & ~TCPC_ROLE_CTRL_CC2);
			else if (cc2 == TYPEC_CC_RP_DEF)
				role_ctrl = FIELD_PREP(TCPC_ROLE_CTRL_CC1,
						       TCPC_ROLE_CTRL_CC_OPEN) |
					    (role_ctrl & ~TCPC_ROLE_CTRL_CC1);
			else
				role_ctrl = 0;

			if (role_ctrl)
				husb311_write8(chip, TCPC_ROLE_CTRL,
					       role_ctrl);
		}
	}

	return tcpci_irq(chip->tcpci);
}

static int husb311_check_revision(struct husb311_chip *chip)
{
	u16 vid, pid;
	int ret;

	ret = regmap_raw_read(chip->data.regmap, TCPC_VENDOR_ID, &vid,
			      sizeof(vid));
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read vendor ID\n");

	if (vid != HUSB311_VID)
		return dev_err_probe(chip->dev, -ENODEV,
				     "unexpected vendor ID 0x%04x\n", vid);

	ret = regmap_raw_read(chip->data.regmap, TCPC_PRODUCT_ID, &pid,
			      sizeof(pid));
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to read product ID\n");

	if (pid != HUSB311_PID)
		return dev_err_probe(chip->dev, -ENODEV,
				     "unexpected product ID 0x%04x\n", pid);

	return 0;
}

static void husb311_unregister_tcpci_port(void *tcpci)
{
	tcpci_unregister_port(tcpci);
}

static int husb311_probe(struct i2c_client *client)
{
	struct husb311_chip *chip;
	int ret;

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;
	chip->data.regmap = devm_regmap_init_i2c(client,
						 &husb311_regmap_config);
	if (IS_ERR(chip->data.regmap))
		return PTR_ERR(chip->data.regmap);

	i2c_set_clientdata(client, chip);
	mutex_init(&chip->lock);
	INIT_DELAYED_WORK(&chip->osc_off_work, husb311_osc_off_work);

	ret = husb311_check_revision(chip);
	if (ret)
		return ret;

	ret = husb311_sw_reset(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to soft reset the chip\n");

	chip->vbus = devm_regulator_get_optional(chip->dev, "vbus");
	if (IS_ERR(chip->vbus)) {
		ret = PTR_ERR(chip->vbus);
		if (ret != -ENODEV)
			return dev_err_probe(chip->dev, ret,
					     "failed to get vbus supply\n");
		chip->vbus = NULL;
	} else {
		chip->data.set_vbus = husb311_set_vbus;
	}

	chip->data.init = husb311_init;

	chip->tcpci = tcpci_register_port(chip->dev, &chip->data);
	if (IS_ERR(chip->tcpci))
		return PTR_ERR(chip->tcpci);

	ret = devm_add_action_or_reset(chip->dev, husb311_unregister_tcpci_port,
				       chip->tcpci);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(chip->dev, client->irq, NULL,
					husb311_irq,
					IRQF_ONESHOT | IRQF_TRIGGER_LOW,
					dev_name(chip->dev), chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to request IRQ\n");

	chip->wakeup = device_property_read_bool(chip->dev, "wakeup-source");
	device_init_wakeup(chip->dev, true);

	return 0;
}

static void husb311_remove(struct i2c_client *client)
{
	struct husb311_chip *chip = i2c_get_clientdata(client);

	/* Stop interrupts before tearing down the tcpci port */
	disable_irq(client->irq);
	cancel_delayed_work_sync(&chip->osc_off_work);

	if (chip->vbus_on)
		regulator_disable(chip->vbus);

	devm_release_action(chip->dev, husb311_unregister_tcpci_port,
			    chip->tcpci);
	device_init_wakeup(chip->dev, false);
}

static int husb311_pm_suspend(struct device *dev)
{
	struct husb311_chip *chip = dev_get_drvdata(dev);
	struct i2c_client *client = to_i2c_client(dev);
	int ret = 0;

	/*
	 * Wake on detach/attach unless the port is actively sourcing VBUS
	 * and the user did not mark the controller as a wakeup source.
	 */
	if (device_may_wakeup(dev) && (!chip->vbus_on || chip->wakeup))
		enable_irq_wake(client->irq);
	else
		disable_irq(client->irq);

	mutex_lock(&chip->lock);
	if (!chip->osc_off && !chip->vbus_on && !chip->sink_on) {
		/*
		 * Vendor code disabled the oscillator unconditionally here,
		 * even while sourcing or sinking VBUS, and relied on a
		 * resume-time chip reset to recover.  Only park it when the
		 * port is completely idle; while active, leave it running.
		 */
		ret = husb311_osc_enable(chip, false);
		if (!ret)
			chip->osc_off = true;
	}
	mutex_unlock(&chip->lock);

	return ret;
}

static int husb311_pm_resume(struct device *dev)
{
	struct husb311_chip *chip = dev_get_drvdata(dev);
	struct i2c_client *client = to_i2c_client(dev);
	u8 filter;
	int ret;

	if (device_may_wakeup(dev) && (!chip->vbus_on || chip->wakeup))
		disable_irq_wake(client->irq);
	else
		enable_irq(client->irq);

	/*
	 * If the chip lost power while suspended, HUSB311_TCPC_FILTER is
	 * back at its power-on default.  Detect that case and reset the
	 * whole TCPC port so the TCPM state machine can re-attach.
	 */
	ret = husb311_read8(chip, HUSB311_TCPC_FILTER, &filter);
	if (ret || filter != HUSB311_FILTER_DEFAULT) {
		ret = husb311_sw_reset(chip);
		if (ret) {
			dev_err(chip->dev,
				"failed to soft reset after resume: %pe\n",
				ERR_PTR(ret));
			return ret;
		}
		tcpm_tcpc_reset(tcpci_get_tcpm_port(chip->tcpci));
	}

	return 0;
}

static const struct dev_pm_ops husb311_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(husb311_pm_suspend, husb311_pm_resume)
};

static const struct i2c_device_id husb311_id[] = {
	{ "husb311" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, husb311_id);

static const struct of_device_id husb311_of_match[] = {
	{ .compatible = "hynetek,husb311" },
	{ }
};
MODULE_DEVICE_TABLE(of, husb311_of_match);

static struct i2c_driver husb311_i2c_driver = {
	.driver = {
		.name = "husb311",
		.pm = pm_ptr(&husb311_pm_ops),
		.of_match_table = husb311_of_match,
	},
	.probe = husb311_probe,
	.remove = husb311_remove,
	.id_table = husb311_id,
};
module_i2c_driver(husb311_i2c_driver);

MODULE_AUTHOR("Wang Jie <dave.wang@rock-chips.com>");
MODULE_DESCRIPTION("Hynetek HUSB311 USB Type-C Port Controller Driver");
MODULE_LICENSE("GPL");
