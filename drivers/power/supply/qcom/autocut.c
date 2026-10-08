// SPDX-License-Identifier: GPL-2.0-only
/*
 * Auto-cut charging module for X00TD (4.19 final)
 * - INPUT_SUSPEND for real power cut
 * - USB plug-in detection: resume on connect (unless ROM limit wants off)
 * - Hysteresis: stop at max, resume at min, do nothing in between
 * - Yields to ROM charge limit (charging_enabled == 0)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

static int max_soc = 100;
static int min_soc = 90;
module_param(max_soc, int, 0644);
MODULE_PARM_DESC(max_soc, "Max SOC to stop charging (default 100)");
module_param(min_soc, int, 0644);
MODULE_PARM_DESC(min_soc, "Min SOC to resume charging (default 90)");

static struct delayed_work autocut_work;
static int last_usb_present = -1;

/* write input_suspend only if it differs from current state */
static void autocut_set_suspend(struct power_supply *batt_psy, int want)
{
	union power_supply_propval val;
	int ret;

	ret = power_supply_get_property(batt_psy,
			POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
	if (!ret && val.intval == want)
		return;

	val.intval = want;
	power_supply_set_property(batt_psy,
			POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
}

static void autocut_work_fn(struct work_struct *work)
{
	struct power_supply *batt_psy, *usb_psy;
	union power_supply_propval val;
	int ret, capacity, usb_present = 0, rom_off = 0;

	/* Check USB present */
	usb_psy = power_supply_get_by_name("usb");
	if (usb_psy) {
		ret = power_supply_get_property(usb_psy, POWER_SUPPLY_PROP_PRESENT, &val);
		if (!ret)
			usb_present = val.intval;
		power_supply_put(usb_psy);
	} else {
		usb_present = last_usb_present > 0 ? 1 : 0;
	}

	batt_psy = power_supply_get_by_name("battery");
	if (!batt_psy)
		goto reschedule;

	ret = power_supply_get_property(batt_psy, POWER_SUPPLY_PROP_CAPACITY, &val);
	if (ret) {
		power_supply_put(batt_psy);
		goto reschedule;
	}
	capacity = val.intval;

	/* ROM charge limit wants charging off? then never resume */
	ret = power_supply_get_property(batt_psy,
			POWER_SUPPLY_PROP_CHARGING_ENABLED, &val);
	if (!ret && val.intval == 0)
		rom_off = 1;

	/* Charger just plugged in: allow charging first */
	if (usb_present == 1 && last_usb_present != 1) {
		if (!rom_off) {
			autocut_set_suspend(batt_psy, 0);
			pr_info("autocut: charger plugged, force resume (soc=%d)\n", capacity);
		}
	}
	/* Charger connected: apply SOC thresholds only */
	else if (usb_present == 1) {
		if (capacity >= max_soc) {
			autocut_set_suspend(batt_psy, 1);
		} else if (capacity <= min_soc && !rom_off) {
			autocut_set_suspend(batt_psy, 0);
		}
		/* else: between min and max, or ROM limit active: do nothing */
	}

	last_usb_present = usb_present;
	power_supply_put(batt_psy);

reschedule:
	schedule_delayed_work(&autocut_work, msecs_to_jiffies(3000));
}

static int __init autocut_init(void)
{
	INIT_DELAYED_WORK(&autocut_work, autocut_work_fn);
	schedule_delayed_work(&autocut_work, msecs_to_jiffies(3000));
	pr_info("autocut: loaded (max_soc=%d, min_soc=%d)\n", max_soc, min_soc);
	return 0;
}

static void __exit autocut_exit(void)
{
	cancel_delayed_work_sync(&autocut_work);
	pr_info("autocut: unloaded\n");
}

module_init(autocut_init);
module_exit(autocut_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Auto-cut charging with plug-in resume support");
MODULE_AUTHOR("X00TD Porter");
