/*
 * FPC1020 Fingerprint sensor device driver
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/platform_device.h>
#include <linux/notifier.h>
#include <linux/kref.h>
#include <linux/bitmap.h>
#include <linux/list.h>
#include <linux/mutex.h>

#define RESET_LOW_SLEEP_MIN_US 5000
#define RESET_LOW_SLEEP_MAX_US (RESET_LOW_SLEEP_MIN_US + 100)
#define RESET_HIGH_SLEEP1_MIN_US 100
#define RESET_HIGH_SLEEP1_MAX_US (RESET_HIGH_SLEEP1_MIN_US + 100)
#define RESET_HIGH_SLEEP2_MIN_US 5000
#define RESET_HIGH_SLEEP2_MAX_US (RESET_HIGH_SLEEP2_MIN_US + 100)

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 25))
#define devm_gpio_free(a, b) NULL
#endif

struct FPS_data {
	struct kref refs;
	unsigned int enabled;
	unsigned int state;
	struct blocking_notifier_head nhead;
};

/*
 * fpsData used to be devm_kzalloc()'d and published straight into a
 * file-scope global, which fpc1020_remove() never cleared.  devm
 * releases the memory as soon as remove returns, but
 * FPS_register_notifier(), FPS_unregister_notifier() and FPS_notify()
 * are EXPORT_SYMBOL_GPL and are reached from other drivers - the
 * synaptics touchscreen calls the first two through weak stubs that
 * resolve to these - so any of them running across a remove was left
 * dereferencing freed memory.
 *
 * It is now reference counted and kfree()'d by its last holder instead,
 * so remove can withdraw it from the global but cannot free it while a
 * caller still holds it.  fpsData is only read or written under
 * fps_data_lock; the operations themselves run without that lock, since
 * blocking_notifier_call_chain() sleeps and may re-enter these APIs -
 * hence a kref rather than a mutex held across the work.  All three
 * entry points are called from process context (sysfs writes, a
 * workqueue worker, a remove path) and never from IRQ, so taking the
 * mutex is always safe.
 */
static DEFINE_MUTEX(fps_data_lock);
static struct FPS_data *fpsData;

static void fps_data_release(struct kref *ref)
{
	struct FPS_data *mdata = container_of(ref, struct FPS_data, refs);

	if (mdata->nhead.head)
		pr_warn("%s: notifier chain still had clients on teardown\n",
			__func__);

	kfree(mdata);
}

/*
 * Take a reference to the published object, or NULL if the sensor is
 * gone.  The read and the kref_get are both under fps_data_lock, and
 * fpc1020_fps_data_unpublish() clears the global under the same lock
 * before dropping the publisher's reference, so there is no window in
 * which a caller can read a pointer whose last reference is already
 * gone.
 */
static struct FPS_data *fps_data_get(void)
{
	struct FPS_data *mdata;

	mutex_lock(&fps_data_lock);
	mdata = fpsData;
	if (mdata)
		kref_get(&mdata->refs);
	mutex_unlock(&fps_data_lock);

	return mdata;
}

static void fps_data_put(struct FPS_data *mdata)
{
	if (mdata)
		kref_put(&mdata->refs, fps_data_release);
}

/*
 * Refuses a second instance rather than replacing the published
 * pointer, so a re-probe cannot leave an older object reachable
 * through the global while a newer one is in use.
 */
static int FPS_init(struct device *dev)
{
	struct FPS_data *mdata;
	int rc = 0;

	mdata = kzalloc(sizeof(*mdata), GFP_KERNEL);
	if (!mdata)
		return -ENOMEM;

	BLOCKING_INIT_NOTIFIER_HEAD(&mdata->nhead);
	kref_init(&mdata->refs);

	mutex_lock(&fps_data_lock);
	if (fpsData) {
		mutex_unlock(&fps_data_lock);
		pr_err("%s: FPS data already published, refusing a second instance\n",
			__func__);
		kfree(mdata);
		return -EBUSY;
	}
	fpsData = mdata;
	mutex_unlock(&fps_data_lock);

	pr_debug("%s: FPS notifier data structure init-ed\n", __func__);
	return rc;
}

static void fpc1020_fps_data_unpublish(void)
{
	struct FPS_data *mdata;

	mutex_lock(&fps_data_lock);
	mdata = fpsData;
	fpsData = NULL;
	mutex_unlock(&fps_data_lock);

	/*
	 * Outside the lock: the object may still be alive because a
	 * caller took a reference before we withdrew it, and freeing it
	 * must not be able to block a concurrent fps_data_get().
	 */
	if (mdata)
		kref_put(&mdata->refs, fps_data_release);
}

int FPS_register_notifier(struct notifier_block *nb,
	unsigned long stype, bool report)
{
	int error;
	struct FPS_data *mdata = fps_data_get();

	if (!mdata)
		return -ENODEV;

	mdata->enabled = (unsigned int)stype;
	pr_info("%s: FPS sensor %lu notifier enabled\n", __func__, stype);

	error = blocking_notifier_chain_register(&mdata->nhead, nb);
	if (!error && report) {
		int state = mdata->state;
		/* send current FPS state on register request */
		blocking_notifier_call_chain(&mdata->nhead,
				stype, (void *)&state);
		pr_debug("%s: FPS reported state %d\n", __func__, state);
	}
	fps_data_put(mdata);
	return error;
}
EXPORT_SYMBOL_GPL(FPS_register_notifier);

int FPS_unregister_notifier(struct notifier_block *nb,
		unsigned long stype)
{
	int error;
	struct FPS_data *mdata = fps_data_get();

	if (!mdata)
		return -ENODEV;

	error = blocking_notifier_chain_unregister(&mdata->nhead, nb);
	pr_debug("%s: FPS sensor %lu notifier unregister\n", __func__, stype);

	if (!mdata->nhead.head) {
		mdata->enabled = 0;
		pr_info("%s: FPS sensor %lu no clients\n", __func__, stype);
	}

	fps_data_put(mdata);
	return error;
}
EXPORT_SYMBOL_GPL(FPS_unregister_notifier);

void FPS_notify(unsigned long stype, int state)
{
	struct FPS_data *mdata = fps_data_get();

	pr_debug("%s: Enter", __func__);

	if (!mdata) {
		pr_err("%s: FPS notifier not initialized yet\n", __func__);
		return;
	} else if (!mdata->enabled) {
		pr_debug("%s: !mdata->enabled", __func__);
		fps_data_put(mdata);
		return;
	}

	pr_debug("%s: FPS current state %d -> (0x%x)\n", __func__,
	       mdata->state, state);

	if (mdata->state != state) {
		mdata->state = state;
		blocking_notifier_call_chain(&mdata->nhead,
					     stype, (void *)&state);
		pr_debug("%s: FPS notification sent\n", __func__);
	} else
		pr_warn("%s: mdata->state==state", __func__);

	fps_data_put(mdata);
}

struct fpc1020_data {
	struct device *dev;
#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	struct device *class_dev;
	/* device number and region minor this instance owns */
	dev_t class_devno;
	int class_minor;
#endif
	struct platform_device *pdev;
	struct notifier_block nb;
	int irq_gpio;
	int irq_num;
	unsigned int irq_cnt;
	int rst_gpio;
	int pwr_gpio;
	int power_enabled;
	unsigned int  rgltr_ctrl_support; //whether regulator control is supported
	struct regulator *pwr_supply;
	int pwr_voltage_range[2];
	int pwr_load[1];
};

static int fpc1020_power_on(struct fpc1020_data *fpc1020)
{
	int rc = 0;
	if(!fpc1020) return 0;

	if (!fpc1020->power_enabled) {
		if(fpc1020->rgltr_ctrl_support && !IS_ERR_OR_NULL(fpc1020->pwr_supply)) {
			rc = regulator_enable(fpc1020->pwr_supply);
			pr_warn(" %s : enable  pwr_supply return %d \n", __func__, rc);
		}
		if (gpio_is_valid(fpc1020->pwr_gpio)) {
			gpio_direction_output(fpc1020->pwr_gpio, 1);
		}
		fpc1020->power_enabled = 1;
		if(fpc1020->rgltr_ctrl_support ||gpio_is_valid(fpc1020->pwr_gpio)){
			if (!of_property_read_bool(fpc1020->dev->of_node, "delay-ctrl-support")) {
				usleep_range(11000,12000);
			}
		}
	}
	return rc;
}

int fpc1020_power_off(struct fpc1020_data *fpc1020)
{
	int rc = 0;
	if(!fpc1020) return 0;
	if (fpc1020->power_enabled) {
		if (fpc1020->rgltr_ctrl_support  && !IS_ERR_OR_NULL(fpc1020->pwr_supply)) {
			rc = regulator_disable(fpc1020->pwr_supply);
			pr_warn(" %s : disable  pwr_supply return %d \n", __func__, rc);
		}
		if (gpio_is_valid(fpc1020->pwr_gpio)) {
			gpio_direction_output(fpc1020->pwr_gpio, 0);
		}
		fpc1020->power_enabled = 0;
	}
	return rc;
}

static int hw_reset(struct fpc1020_data *fpc1020)
{
	int irq_gpio;
	struct device *dev = fpc1020->dev;
	int rc = 0;

	if (!gpio_is_valid(fpc1020->rst_gpio)) {
		dev_warn(dev, "reset pin is invalid\n");
		goto exit;
	}

	rc = gpio_direction_output(fpc1020->rst_gpio, 1);
	if (rc)
		goto exit;
	usleep_range(RESET_HIGH_SLEEP1_MIN_US, RESET_HIGH_SLEEP1_MAX_US);

	rc = gpio_direction_output(fpc1020->rst_gpio, 0);
	if (rc)
		goto exit;
	usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);

	rc = gpio_direction_output(fpc1020->rst_gpio, 1);
	if (rc)
		goto exit;
	usleep_range(RESET_HIGH_SLEEP2_MIN_US, RESET_HIGH_SLEEP2_MAX_US);

	irq_gpio = gpio_get_value(fpc1020->irq_gpio);
	dev_info(dev, "IRQ after reset %d\n", irq_gpio);

exit:
	return rc;
}
#ifdef SUPPORT_PIN_CTRL
static void fpc_pinctrl_on(struct device *dev)
{
	struct pinctrl *ptl = NULL;
	struct pinctrl_state *ptl_state = NULL;

        pr_info("fpc fpc_pinctrl_on begin\n");
    	ptl = devm_pinctrl_get(dev);
	ptl_state = pinctrl_lookup_state(ptl, "fpc_irq_en");
	pinctrl_select_state(ptl, ptl_state);
        ptl_state = pinctrl_lookup_state(ptl, "fpc_vdd_on");
        pinctrl_select_state(ptl, ptl_state);
        ptl_state = pinctrl_lookup_state(ptl, "fpc_rst_hi");
        pinctrl_select_state(ptl, ptl_state);
        msleep(10);
        ptl_state = pinctrl_lookup_state(ptl, "fpc_rst_lo");
        pinctrl_select_state(ptl, ptl_state);
        msleep(5);
        ptl_state = pinctrl_lookup_state(ptl, "fpc_rst_hi");
        pinctrl_select_state(ptl, ptl_state);
        msleep(3);
    	devm_pinctrl_put(ptl);
        pr_info("fpc fpc_pinctrl_on end\n");
}
#endif
static ssize_t hw_reset_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int rc;
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	dev_info(dev," %s : hw_reset_set %s\n", __func__, (buf == NULL) ? "":buf);
	if (!strncmp(buf, "reset", strlen("reset"))) {
		rc = hw_reset(fpc1020);
	} else if (!strncmp(buf, "poweroff", strlen("poweroff"))) {
		rc = fpc1020_power_off(fpc1020);
	} else if (!strncmp(buf, "poweron", strlen("poweron"))) {
		rc = fpc1020_power_on(fpc1020);
	}
  #ifdef SUPPORT_PIN_CTRL
  	  else if (!strncmp(buf, "pinctrl", strlen("pinctrl"))) {
		fpc_pinctrl_on(dev);
		pr_info("fpc IRQ after reset %d\n", gpio_get_value(fpc1020->irq_gpio));
                rc = count;
	}
  #endif
  	  else {
		return -EINVAL;
	}
	return rc ? rc : count;
}
static DEVICE_ATTR(hw_reset, S_IWUSR, NULL, hw_reset_set);

static ssize_t dev_enable_set(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	int state = (*buf == '1') ? 1 : 0;

	FPS_notify(0xbeef, state);
	dev_dbg(fpc1020->dev, "%s state = %d\n", __func__, state);
	return 1;
}
static DEVICE_ATTR(dev_enable, S_IWUSR | S_IWGRP, NULL, dev_enable_set);

static ssize_t irq_get(struct device *device,
		       struct device_attribute *attribute,
		       char *buffer)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(device);
	int irq = gpio_get_value(fpc1020->irq_gpio);

	return scnprintf(buffer, PAGE_SIZE, "%i\n", irq);
}
static DEVICE_ATTR(irq, S_IRUSR | S_IRGRP, irq_get, NULL);

static ssize_t irq_cnt_get(struct device *device,
		       struct device_attribute *attribute,
		       char *buffer)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(device);

	return scnprintf(buffer, PAGE_SIZE, "%u\n", fpc1020->irq_cnt);
}
static DEVICE_ATTR(irq_cnt, S_IRUSR, irq_cnt_get, NULL);

#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
/* Attribute: vendor (RO) */
static ssize_t vendor_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "fpc");
}
static DEVICE_ATTR_RO(vendor);

static ssize_t modalias_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "fpc1020");
}
static DEVICE_ATTR_RO(modalias);
#endif

static struct attribute *attributes[] = {
	&dev_attr_dev_enable.attr,
	&dev_attr_irq.attr,
	&dev_attr_irq_cnt.attr,
	&dev_attr_hw_reset.attr,
#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	&dev_attr_vendor.attr,
	&dev_attr_modalias.attr,
#endif
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
static const struct attribute_group *attribute_groups[] = {
	&attribute_group,
	NULL
};
#endif

#define MAX_UP_TIME (1 * MSEC_PER_SEC)

static irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;

	pm_wakeup_event(fpc1020->dev, MAX_UP_TIME);
	dev_dbg(fpc1020->dev, "%s\n", __func__);
	fpc1020->irq_cnt++;
#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	sysfs_notify(&fpc1020->class_dev->kobj, NULL, dev_attr_irq.attr.name);
#else
	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);
#endif
	return IRQ_HANDLED;
}

static int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
		const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc;

	*gpio = of_get_named_gpio(np, label, 0);
	/*
	 * of_find_gpiochip_by_xlate() reports a chip that has not been
	 * registered yet as -EPROBE_DEFER, and of_get_named_gpio()
	 * propagates it.  That is a "try again later", not a bad
	 * property, so it has to reach the caller unchanged or the driver
	 * binds once against a controller that was not ready and never
	 * retries.  Every other negative is a genuine problem and keeps
	 * the existing -EINVAL.
	 */
	if (*gpio == -EPROBE_DEFER) {
		dev_info(dev, "gpio %s not ready, deferring probe\n", label);
		return -EPROBE_DEFER;
	}
	if (!gpio_is_valid(*gpio)) {
		dev_err(dev, "gpio %s is invalid\n", label);
		return -EINVAL;
	}
	rc = devm_gpio_request(dev, *gpio, label);
	if (rc) {
		dev_err(dev, "failed to request gpio %d\n", *gpio);
		return rc;
	}
	dev_dbg(dev, "%s %d\n", label, *gpio);
	return 0;
}

#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
#define MAX_INSTANCE	5
#define MAJOR_BASE	32

/*
 * The character-device region and the class are module-wide, not
 * per-instance, and each instance takes one distinct minor out of them.
 * Previously alloc_chrdev_region() ran on every create, so a second
 * instance reserved a second range and overwrote the shared dev_no, and
 * teardown destroyed the shared class and released one of the five
 * minors the allocation had reserved.  One region, allocated on the
 * first instance and released on the last, with a bitmap recording
 * which minors are taken, keeps instance A's removal from taking
 * anything instance B is still using.
 */
static DEFINE_MUTEX(fpc1020_class_lock);
static struct class *fingerprint_class;
static dev_t fpc1020_region;
static unsigned long fpc1020_minor_taken;
static unsigned int fpc1020_instance_count;

static int fpc1020_create_sysfs(struct fpc1020_data *fpc1020, bool create)
{
	struct device *dev = fpc1020->dev;
	int rc = 0;
	int minor;

	mutex_lock(&fpc1020_class_lock);

	if (create) {
		if (fpc1020_instance_count == 0) {
			rc = alloc_chrdev_region(&fpc1020_region, MAJOR_BASE,
						  MAX_INSTANCE, "fpc");
			if (rc < 0) {
				dev_err(dev,
					"%s alloc fingerprint region failed.\n",
					__func__);
				goto out_unlock;
			}

			fingerprint_class = class_create(THIS_MODULE, "fingerprint");
			if (IS_ERR(fingerprint_class)) {
				rc = PTR_ERR(fingerprint_class);
				fingerprint_class = NULL;
				goto unregister_region;
			}
		}

		minor = find_first_zero_bit(&fpc1020_minor_taken, MAX_INSTANCE);
		if (minor >= MAX_INSTANCE) {
			dev_err(dev, "%s no free minor left\n", __func__);
			rc = -ENODEV;
			goto release_class;
		}
		__set_bit(minor, &fpc1020_minor_taken);

		fpc1020->class_minor = minor;
		fpc1020->class_devno = MKDEV(MAJOR(fpc1020_region), minor);

		fpc1020->class_dev = device_create_with_groups(fingerprint_class,
				NULL, fpc1020->class_devno, fpc1020,
				attribute_groups, "fpc1020");
		if (IS_ERR(fpc1020->class_dev)) {
			dev_err(dev, "%s create fingerprint class device failed.\n",
				__func__);
			rc = PTR_ERR(fpc1020->class_dev);
			fpc1020->class_dev = NULL;
			__clear_bit(minor, &fpc1020_minor_taken);
			goto release_class;
		}

		fpc1020_instance_count++;
		goto out_unlock;
	}

	/*
	 * Teardown.  Only ever destroys what this instance created: its
	 * own device node and its own minor.  The class and the region go
	 * away with the last instance, not the first.
	 */
	if (fpc1020->class_dev) {
		device_destroy(fingerprint_class, fpc1020->class_devno);
		fpc1020->class_dev = NULL;
	}
	/*
	 * The count is decremented only by an instance that actually holds
	 * a minor, so a repeated teardown is idempotent.  class_dev and
	 * class_minor already were, but decrementing unconditionally would
	 * have let a second call drive the count to zero and take the class
	 * and region away from a live peer.
	 */
	if (fpc1020->class_minor >= 0 && fpc1020->class_minor < MAX_INSTANCE) {
		__clear_bit(fpc1020->class_minor, &fpc1020_minor_taken);
		fpc1020->class_minor = -1;
		if (fpc1020_instance_count)
			fpc1020_instance_count--;
	}

	if (fpc1020_instance_count == 0) {
		class_destroy(fingerprint_class);
		fingerprint_class = NULL;
		goto unregister_region;
	}
	goto out_unlock;

release_class:
	if (fpc1020_instance_count == 0 && fingerprint_class) {
		class_destroy(fingerprint_class);
		fingerprint_class = NULL;
	}
unregister_region:
	if (fpc1020_instance_count == 0 && MAJOR(fpc1020_region)) {
		unregister_chrdev_region(fpc1020_region, MAX_INSTANCE);
		fpc1020_region = 0;
	}
out_unlock:
	mutex_unlock(&fpc1020_class_lock);
	return rc;
}
#endif

static int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int rc = 0;
	int irqf;
	struct device_node *np = dev->of_node;
	struct fpc1020_data *fpc1020 = devm_kzalloc(dev, sizeof(*fpc1020),
			GFP_KERNEL);
	if (!fpc1020) {
		rc = -ENOMEM;
		goto exit;
	}
  #ifdef SUPPORT_PIN_CTRL
	if(of_property_read_bool(np,"fpc_pinctrl_on")) {
            fpc_pinctrl_on(dev);
	}
  #endif
	rc = FPS_init(dev);
	if (rc) {
		dev_err(dev, "FPS notifier init failed: %d\n", rc);
		goto exit;
	}

	fpc1020->dev = dev;
#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	fpc1020->class_minor = -1;
#endif
	dev_set_drvdata(dev, fpc1020);
	fpc1020->pdev = pdev;
	fpc1020->power_enabled = 0;
	fpc1020->pwr_supply = NULL;
	if (!np) {
		dev_err(dev, "no of node found\n");
		rc = -EINVAL;
		goto exit;
	}
	fpc1020->pwr_gpio = of_get_named_gpio(np, "fp-gpio-ven", 0);
	if (fpc1020->pwr_gpio == -EPROBE_DEFER) {
		/* Controller not registered yet; retry rather than fall
		 * back, or the sensor is wired with no power control. */
		pr_info("pwr gpio not ready, deferring probe\n");
		rc = -EPROBE_DEFER;
		goto exit;
	}
	if (fpc1020->pwr_gpio < 0) {
		pr_warn("failed to get pwr gpio!\n");
		fpc1020->pwr_gpio = -1;
	} else {
		if (gpio_is_valid(fpc1020->pwr_gpio)) {
			rc = devm_gpio_request(dev, fpc1020->pwr_gpio, "fpc_pwr");
			if (rc) {
				pr_err("failed to request pwr gpio, rc = %d\n", rc);
				goto err_pwr;
			}
			gpio_direction_output(fpc1020->pwr_gpio, 1);
		}
	}
	if(of_property_read_bool(np,"rgltr-ctrl-support")) {
		fpc1020->rgltr_ctrl_support = 1;
	} else {
		fpc1020->rgltr_ctrl_support = 0;
		pr_err("No regulator control parameter defined\n");
	}
	if (fpc1020->rgltr_ctrl_support) {
		fpc1020->pwr_supply = regulator_get(dev, "fp,vdd");
		if (IS_ERR_OR_NULL(fpc1020->pwr_supply)) {
			fpc1020->pwr_supply = NULL;
			fpc1020->rgltr_ctrl_support = 0;
			pr_warn("Unable to get fp,vdd-regulator");
		} else {
			rc = of_property_read_u32_array(np, "fp,voltage-range", fpc1020->pwr_voltage_range, 2);
			if (rc) {
				fpc1020->pwr_voltage_range[0] = -1;
				fpc1020->pwr_voltage_range[1] = -1;
			}
			if (regulator_count_voltages(fpc1020->pwr_supply) > 0) {
				if((fpc1020->pwr_voltage_range[0] >0) && (fpc1020->pwr_voltage_range[1] > 0))
					rc = regulator_set_voltage(fpc1020->pwr_supply, fpc1020->pwr_voltage_range[0], fpc1020->pwr_voltage_range[1]);
				if (rc) {
					pr_warn(" %s : set vdd regulator voltage failed %d \n", __func__, rc);
				}
			}
		}
	}

	fpc1020_power_on(fpc1020);

	rc = fpc1020_request_named_gpio(fpc1020, "irq",
			&fpc1020->irq_gpio);
	if (rc)
		goto exit;
	/*
	 * After the check, not before: on failure irq_gpio still holds a
	 * negative errno, and gpio_direction_input() on one of those only
	 * logs "invalid GPIO desc" and returns an error nobody reads.
	 */
	gpio_direction_input(fpc1020->irq_gpio);

	rc = fpc1020_request_named_gpio(fpc1020, "rst",
			&fpc1020->rst_gpio);
	if (rc)
		fpc1020->rst_gpio = -EINVAL;

	if (gpio_is_valid(fpc1020->rst_gpio)) {
		if (!of_property_read_bool(np, "delay-ctrl-support")) {
			usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);
		}
		rc = gpio_direction_output(fpc1020->rst_gpio, 1);
		if (rc) {
			dev_err(dev, "cannot set reset pin direction\n");
			goto exit;
		}
	}

	rc = device_init_wakeup(fpc1020->dev, true);
	if (rc)
		goto exit;

#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	rc = fpc1020_create_sysfs(fpc1020, true);
#else
	rc = sysfs_create_group(&dev->kobj, &attribute_group);
#endif
	if (rc) {
		dev_err(dev, "could not create sysfs\n");
		goto exit;
	}

	fpc1020->irq_cnt = 0;
	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;

	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
			NULL, fpc1020_irq_handler, irqf,
			dev_name(dev), fpc1020);
	if (rc) {
		dev_err(dev, "could not request irq %d\n",
				gpio_to_irq(fpc1020->irq_gpio));
		goto irq_exit;
	}
	dev_dbg(dev, "requested irq %d\n", gpio_to_irq(fpc1020->irq_gpio));

	/* Request that the interrupt should be wakeable */
	enable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));

	dev_info(dev, "%s: ok\n", __func__);

	return 0;

irq_exit:
#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	fpc1020_create_sysfs(fpc1020, false);
#else
	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
#endif
exit:
err_pwr:
	if(!IS_ERR_OR_NULL(fpc1020->pwr_supply))
	{
		pr_info(" %s : devm_regulator_put \n", __func__);
		regulator_disable(fpc1020->pwr_supply);
		regulator_put(fpc1020->pwr_supply);
		fpc1020->pwr_supply= NULL;
	}

	if (gpio_is_valid(fpc1020->pwr_gpio)) {
		devm_gpio_free(fpc1020->dev,fpc1020->pwr_gpio);
		fpc1020->pwr_gpio = -1;
		pr_info("remove pwr_gpio success\n");
	}
	return rc;
}

static int fpc1020_remove(struct platform_device *pdev)
{
	struct  fpc1020_data *fpc1020 = dev_get_drvdata(&pdev->dev);

	/*
	 * Withdraw the notifier object first, before anything else is
	 * torn down and before devm releases fpc1020.  This does not free
	 * it if a caller already holds a reference; it only stops new
	 * ones appearing, so the last holder frees it.
	 */
	fpc1020_fps_data_unpublish();

	disable_irq(gpio_to_irq(fpc1020->irq_gpio));
#ifdef CONFIG_INPUT_MISC_FPC1020_SAVE_TO_CLASS_DEVICE
	fpc1020_create_sysfs(fpc1020, false);
#else
	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
#endif

	device_init_wakeup(fpc1020->dev, false);
	devm_free_irq(fpc1020->dev, gpio_to_irq(fpc1020->irq_gpio),fpc1020);
	if(fpc1020->rgltr_ctrl_support ||gpio_is_valid(fpc1020->pwr_gpio)){
		fpc1020_power_off(fpc1020);
	}
	if (gpio_is_valid(fpc1020->irq_gpio)) {
		devm_gpio_free(fpc1020->dev, fpc1020->irq_gpio);
		fpc1020->irq_gpio = -1;
	}
	if (gpio_is_valid(fpc1020->rst_gpio)) {
		devm_gpio_free(fpc1020->dev, fpc1020->rst_gpio);
		fpc1020->rst_gpio = -1;

	}
	if (fpc1020->rgltr_ctrl_support && !IS_ERR_OR_NULL(fpc1020->pwr_supply))
	{
		regulator_put(fpc1020->pwr_supply);
		fpc1020->pwr_supply= NULL;
		pr_info(" %s : regulator_put vdd \n", __func__);
	}
	if (gpio_is_valid(fpc1020->pwr_gpio)) {
		devm_gpio_free(fpc1020->dev,fpc1020->pwr_gpio);
		fpc1020->pwr_gpio = -1;
		pr_info("remove pwr_gpio success\n");
	}
	dev_info(&pdev->dev, "%s\n", __func__);
	return 0;
}

static int fpc1020_suspend(struct device *dev)
{
	return 0;
}

static int fpc1020_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops fpc1020_pm_ops = {
	.suspend = fpc1020_suspend,
	.resume = fpc1020_resume,
};

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{}
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name	= "fpc1020",
		.owner	= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
#if defined(CONFIG_PM)
		.pm = &fpc1020_pm_ops,
#endif
	},
	.probe		= fpc1020_probe,
	.remove		= fpc1020_remove,
};

static int __init fpc1020_init(void)
{
	int rc = platform_driver_register(&fpc1020_driver);

	if (!rc)
		pr_debug("%s OK\n", __func__);
	else
		pr_err("%s %d\n", __func__, rc);
	return rc;
}

static void __exit fpc1020_exit(void)
{
	pr_debug("%s\n", __func__);
	platform_driver_unregister(&fpc1020_driver);
}

module_init(fpc1020_init);
module_exit(fpc1020_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
