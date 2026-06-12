// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  aorus-laptop.c - AORUS laptop WMI driver
 *
 *  Copyright (C) 2023 Albert Tang
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/umh.h>
#include <linux/workqueue.h>
#include <linux/wmi.h>

#define GIGABYTE_LAPTOP_VERSION "0.01"
#define GIGABYTE_LAPTOP_FILE  KBUILD_MODNAME

MODULE_AUTHOR("Albert Tang");
MODULE_DESCRIPTION("Gigabyte laptop WMI driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(GIGABYTE_LAPTOP_VERSION);

/* _SB_.PCI0.AMW0._WDG */
#define WMI_EVENT "ABBC0F72-8EA1-11D1-00A0-C90629100000" // Hopefully, it's used for hotkeys
#define WMI_METHOD_WMBC "ABBC0F6F-8EA1-11D1-00A0-C90629100000" // Seems to only return values
#define WMI_METHOD_WMBD "ABBC0F75-8EA1-11D1-00A0-C90629100000" // Will probably do most of the work.

/* WMI method arguments */
// Not supported by Aero 14 W
#define GPU_QBOOST       0x51
#define FAN_SILENT_MODE  0x57
#define CHARGING_MODE    0x64
#define CHARGING_LIMIT   0x65
// Supported by Aero 14 W
#define FAN_CUSTOM_MODE  0x67
#define FAN_INDEX_VALUE  0x68
#define FAN_FIXED_MODE   0x6A
#define FAN_CUSTOM_SPEED 0x6B
#define BATT_CYCLE2      0x6D
#define BATT_CYCLE       0x6E
#define FAN_AUTO_MODE    0x70
#define FAN_GAMING_MODE  0x71
// Gigabyte Gaming models (e.g. A16 CWH)
#define FAN_TURBO        0x7D // EC TFAN bit, max fan speed
#define DYN_BOOST        0xE7 // NPCF dynamic boost (NOTE: write 0 = enable)
#define PERF_MODE        0xED // NPCF perf mode, controls GPU TGP/CTGP boost
#define USB_SLEEP        0x7A
#define USB_HIBERNATE    0x7B
#define WIFI_TOGGLE      0xC2
#define TOUCHPAD_ENABLED 0xCA
#define TEMP_CPU         0xE1
#define TEMP_GPU         0xE2
#define FAN_CPU_RPM      0xE4
#define FAN_GPU_RPM      0xE5
#define FAN_THREE_RPM    0xE8 // 2023 AORUS 17
#define FAN_FOUR_RPM     0xE9 // 2023 AORUS 17X
#define FAN_SILENT_OLD   0xFA // Older Aero and P-series models

// Fan curves
#define FAN_CURVE_POINTS 15
#define FAN_DUTY_MAX     0xE5 // 100% custom fan duty

/* Gaming-mode thermal boost (A16 CWH): poll GPU temp + RPM, ramp custom duty */
#define FAN_GAMING_BOOST_POLL_MS  2000
#define FAN_GAMING_BOOST_LEVELS   4
#define FAN_GAMING_BOOST_IDLE_TEMP 45

struct fan_curve_data {
	u8 temperature[FAN_CURVE_POINTS];
	u8 speed[FAN_CURVE_POINTS];
};

struct gigabyte_laptop_wmi {
	struct platform_device *pdev;
	struct device *hwmon_dev;
	struct fan_curve_data fan_curve;

	int fan_mode;
	int fan_custom_display_speed;
	int fan_custom_internal_speed;
	int charge_mode;
	int charge_limit;
	int gpu_boost;
	int fan_curve_index;
	int perf_mode;

	u8 fan_silent_method;
	u8 debug_method;
	u8 dual_fan_speed_enabled;
	u8 gaming_family; // Gigabyte Gaming models (e.g. A16 CWH)
	u8 fan_turbo_active;
	int fan_turbo_saved_mode;
	int fan_turbo_saved_display_speed;
	int fan_turbo_saved_internal_speed;
	struct work_struct fan_turbo_work;
	struct completion fan_turbo_done;
	struct device *fan_turbo_dev;
	bool fan_turbo_work_enable;
	int fan_turbo_work_result;
	u8 fan_gaming_boost_enabled;
	u8 fan_gaming_boost_level;
	u8 fan_gaming_boost_active;
	struct delayed_work fan_gaming_boost_work;
	struct device *fan_gaming_boost_dev;
};

static struct platform_device *platform_device;

static u8 fan_modes[] = {
	0,
	FAN_SILENT_MODE,
	FAN_GAMING_MODE,
	FAN_CUSTOM_MODE,
	FAN_AUTO_MODE,
	FAN_FIXED_MODE
};

/* WMI methods ********************************************/

/* WMBC method (checks value in EC) */
static int gigabyte_laptop_get_devstate2(u32 method_id, u32 arg2, int *result)
{
	union acpi_object *obj;
	acpi_status status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer input = { sizeof(arg2), &arg2 };

	status = wmi_evaluate_method(WMI_METHOD_WMBC, 0, method_id, &input, &buffer);
	if (ACPI_FAILURE(status))
		return -1;

	obj = buffer.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER)
		*result = obj->integer.value;
	// TODO: Fix copying data to pointer
	else if (obj && obj->type == ACPI_TYPE_BUFFER) {
		if (obj->buffer.length == 0) {
			kfree(obj);
			return -ENODATA;
		}
		/*pr_info("This works\n");
		pr_info("ACPI buffer length: %u\n", obj->buffer.length);
		pr_info("ACPI buffer contents: \n");

		for (int i = 0; i < obj->buffer.length; i++) {
			pr_info("%02x \n", obj->buffer.pointer[i]);
		}*/
		//memcpy(result, obj->buffer.pointer, obj->buffer.length);
	}
	else {
		kfree(obj);
		return -EINVAL;
	}
	kfree(obj);
	return 0;
}

static int gigabyte_laptop_get_devstate(u32 method_id, int *result) {
	return gigabyte_laptop_get_devstate2(method_id, 0, result);
}

/* WMBD method (sets value in EC) */
static int gigabyte_laptop_set_devstate(u32 method_id, u32 arg2, int *result)
{
	union acpi_object *obj;
	acpi_status status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer input = { sizeof(arg2), &arg2 };

	status = wmi_evaluate_method(WMI_METHOD_WMBD, 0, method_id, &input, &buffer);
	if (ACPI_FAILURE(status))
		return -1;

	obj = buffer.pointer;
	// Some methods (e.g. 0x7D turbo fan) perform the write without
	// returning a value; treat any non-integer reply as success.
	if (obj && obj->type == ACPI_TYPE_INTEGER)
		*result = obj->integer.value;
	kfree(obj);
	return 0;
}

/* hwmon **************************************************/

/*
 * Helper method. Reverses byte order of fan RPM.
 * This is needed, since the embedded controller stores the value in big-endian
 * while x86 is little-endian.
 */
static u16 convert_fan_rpm(int val)
{
	u16 fan_rpm = val;
	return rol16(fan_rpm, 8);
}

static umode_t gigabyte_laptop_hwmon_is_visible(const void *data, enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	switch (type) {
		case hwmon_temp:
			switch (attr) {
				case hwmon_temp_input:
					return 0444;
				default:
					break;
			}
			break;
		case hwmon_fan:
			switch (attr) {
				case hwmon_fan_input:
					return 0444;
				default:
					break;
			}
			break;
		default:
			break;
	}
	return 0;
}

static int gigabyte_laptop_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
					u32 attr, int channel, long *val)
{
	int ret, output;
	u8 result;
	u8 fan_channels[] = { FAN_CPU_RPM, FAN_GPU_RPM, FAN_THREE_RPM, FAN_FOUR_RPM };

	switch (type) {
		case hwmon_temp:
			switch (channel) {
				case 0:
					ret = gigabyte_laptop_get_devstate(TEMP_CPU, &output);
					if (ret)
						break;
					*val = output * 1000;
					break;
				case 1:
					ret = gigabyte_laptop_get_devstate(TEMP_GPU, &output);
					if (ret)
						break;
					*val = output * 1000;
					break;
				case 2:
					// Motherboard temp cannot be read through WMI
					ret = ec_read(0x62, &result);
					if (ret)
						break;
					*val = result * 1000;
					break;
				default:
					*val = 0;
					break;
			}
			break;
		case hwmon_fan:
			ret = gigabyte_laptop_get_devstate(fan_channels[channel], &output);
			if (ret)
				break;
			// Gigabyte Gaming laptops store fan RPM in little-endian
			if (!strcmp(dmi_get_system_info(DMI_PRODUCT_FAMILY),"GIGABYTE GAMING"))
				*val = output;
			else
				*val = convert_fan_rpm(output);
			break;
		default:
			break;
	}
	return 0;
}

static const struct hwmon_channel_info *gigabyte_laptop_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
				HWMON_T_INPUT,
				HWMON_T_INPUT,
				HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(fan,
				HWMON_F_INPUT,
				HWMON_F_INPUT,
				HWMON_F_INPUT,
				HWMON_F_INPUT),
	NULL
};

static const struct hwmon_ops gigabyte_laptop_hwmon_ops = {
	.read = gigabyte_laptop_hwmon_read,
	.is_visible = gigabyte_laptop_hwmon_is_visible,
};

static const struct hwmon_chip_info gigabyte_laptop_chip_info = {
	.ops = &gigabyte_laptop_hwmon_ops,
	.info = gigabyte_laptop_hwmon_info,
};

/* sysfs **************************************************/

/*
 * Fan mode.
 * 0 = normal fan mode
 * 1 = silent fan mode
 * 2 = gaming fan mode
 * 3 = custom fan mode
 * 4 = auto-maximum mode (requires custom mode)
 * 5 = fixed speed mode (requires custom mode)
 */
static int disable_custom_fan_mode(int mode)
{
	int ret, result;

	if (mode == 5) {
		ret = gigabyte_laptop_set_devstate(FAN_FIXED_MODE, 0, &result);
		if (ret)
			return ret;
	} else if (mode == 4) {
		// Auto-maximum mode can only be turned off through gaming or silent mode
		ret = gigabyte_laptop_set_devstate(FAN_GAMING_MODE, 0, &result);
		if (ret)
			return ret;
	}

	ret = gigabyte_laptop_set_devstate(FAN_CUSTOM_MODE, 0, &result);
	if (ret)
		return ret;

	return 0;
}

static int set_fan_mode(struct gigabyte_laptop_wmi *gigabyte, u32 fan_mode)
{
	int ret, result;

	if (fan_mode == FAN_FIXED_MODE || fan_mode == FAN_AUTO_MODE) {
		if (gigabyte->fan_mode < 3) { // If custom mode is off, enable it
			if (gigabyte->fan_mode > 0) {
				ret = gigabyte_laptop_set_devstate(fan_modes[gigabyte->fan_mode], 0, &result);
				if (ret)
					return ret;
			}

			ret = gigabyte_laptop_set_devstate(FAN_CUSTOM_MODE, 1, &result);
			if (ret)
				return ret;
		}

		if (gigabyte->fan_mode > 3) { // Fixed or auto mode active
			if (gigabyte->fan_mode == 4) {
				// Auto-maximum mode can only be turned off through gaming or silent mode
				ret = gigabyte_laptop_set_devstate(FAN_GAMING_MODE, 0, &result);
				if (ret)
					return ret;
			} else {
				ret = gigabyte_laptop_set_devstate(fan_modes[gigabyte->fan_mode], 0, &result);
				if (ret)
					return ret;
			}
		}

		if (fan_mode == FAN_AUTO_MODE) {
			ret = gigabyte_laptop_set_devstate(fan_mode, gigabyte->fan_custom_internal_speed, &result);
			if (ret)
				return ret;
		} else {
			ret = gigabyte_laptop_set_devstate(fan_mode, 1, &result);
			if (ret)
				return ret;
		}
	} else if (fan_mode == FAN_CUSTOM_MODE) {
		if (gigabyte->fan_mode > 3) {
			pr_warn("Custom mode is already enabled\n");
			return 0;
		} else if (gigabyte->fan_mode > 0) {
			ret = gigabyte_laptop_set_devstate(fan_modes[gigabyte->fan_mode], 0, &result);
			if (ret)
				return ret;
		}

		ret = gigabyte_laptop_set_devstate(FAN_CUSTOM_MODE, 1, &result);
		if (ret)
			return ret;
	} else {
		if (gigabyte->fan_mode >= 3) { // Disable custom mode first. Will revert to normal mode.
			ret = disable_custom_fan_mode(gigabyte->fan_mode);
			if (ret)
				return ret;
		} else if (gigabyte->fan_mode > 0) {
				ret = gigabyte_laptop_set_devstate(fan_modes[gigabyte->fan_mode], 0, &result);
				if (ret)
					return ret;
		}

		if (fan_mode != 0) {
			ret = gigabyte_laptop_set_devstate(fan_mode, 1, &result);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static ssize_t fan_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->fan_mode);
}

static ssize_t fan_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int fan_mode = 0;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtouint(buf, 0, &fan_mode);
	if (ret) {
		pr_err("kstrtouint failed\n");
		return ret;
	}

	gigabyte = dev_get_drvdata(dev);

	if (gigabyte->fan_mode == fan_mode) {
		int tenf;

		/*
		 * Custom mode can look enabled in software while TENF is off in
		 * the EC (e.g. after a failed turbo attempt). Re-apply.
		 */
		if (fan_mode == 3) {
			ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_MODE, &tenf);
			if (!ret && tenf)
				return count;
		} else {
			pr_warn("Already set to that fan mode\n");
			return count;
		}
	}

	if (fan_mode > 5) {
		pr_err("Invalid fan mode\n");
		return -EINVAL;
	} else {
		ret = set_fan_mode(gigabyte, fan_modes[fan_mode]);
		if (ret)
			return ret;
	}

	gigabyte->fan_mode = fan_mode;

	if (gigabyte->fan_gaming_boost_enabled && fan_mode != 2) {
		cancel_delayed_work_sync(&gigabyte->fan_gaming_boost_work);
		gigabyte->fan_gaming_boost_enabled = 0;
		gigabyte->fan_gaming_boost_level = 0;
		gigabyte->fan_gaming_boost_active = 0;
	}

	return count;
}

static int gigabyte_set_fan_custom_speed(struct gigabyte_laptop_wmi *gigabyte,
					 u8 real_speed, int display_speed)
{
	int ret, output;

	ret = gigabyte_laptop_set_devstate(FAN_CUSTOM_SPEED, real_speed, &output);
	if (ret)
		return ret;

	if (gigabyte->dual_fan_speed_enabled) {
		ret = ec_write(0xB1, real_speed);
		if (ret)
			return ret;
	}

	gigabyte->fan_custom_display_speed = display_speed;
	gigabyte->fan_custom_internal_speed = real_speed;
	return 0;
}

/*
 * Custom fan speed. Only works if custom mode is enabled.
 * Must be in multiples of five, between 25 and 100.
 */
static ssize_t fan_custom_speed_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->fan_custom_display_speed);
}

static ssize_t fan_custom_speed_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int speed;
	u8 real_speed;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtouint(buf, 0, &speed);
	if (ret)
		return ret;

	if ((speed < 25 || speed > 100) || speed % 5 != 0) {
		pr_warn("Invalid custom fan speed: Must be a multiple of 5 and between 25 and 100\n");
		return -EINVAL;
	}

	if (speed == 25)
		real_speed = 0x39;
	else if (speed == 30)
		real_speed = 0x44;
	else if (speed == 35)
		real_speed = 0x50;
	else if (speed == 40)
		real_speed = 0x5B;
	else if (speed == 45)
		real_speed = 0x67;
	else if (speed == 50)
		real_speed = 0x72;
	else if (speed == 55)
		real_speed = 0x7D;
	else if (speed == 60)
		real_speed = 0x89;
	else if (speed == 65)
		real_speed = 0x94;
	else if (speed == 70)
		real_speed = 0xA0;
	else if (speed == 75)
		real_speed = 0xAB;
	else if (speed == 80)
		real_speed = 0xB7;
	else if (speed == 85)
		real_speed = 0xC2;
	else if (speed == 90)
		real_speed = 0xCE;
	else if (speed == 95)
		real_speed = 0xD9;
	else if (speed == 100)
		real_speed = 0xE5;

	gigabyte = dev_get_drvdata(dev);
	ret = gigabyte_set_fan_custom_speed(gigabyte, real_speed, speed);
	if (ret)
		return ret;
	return count;
}

static u8 gigabyte_fan_display_to_duty(int display)
{
	switch (display) {
	case 25: return 0x39;
	case 30: return 0x44;
	case 35: return 0x50;
	case 40: return 0x5B;
	case 45: return 0x67;
	case 50: return 0x72;
	case 55: return 0x7D;
	case 60: return 0x89;
	case 65: return 0x94;
	case 70: return 0xA0;
	case 75: return 0xAB;
	case 80: return 0xB7;
	case 85: return 0xC2;
	case 90: return 0xCE;
	case 95: return 0xD9;
	case 100: return FAN_DUTY_MAX;
	default: return 0;
	}
}

/*
 * Charge mode.
 * 0 = default mode
 * 1 = custom mode
 */
static ssize_t charge_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->charge_mode);
}

static ssize_t charge_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, output;
	unsigned int mode;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtouint(buf, 0, &mode);
	if (ret)
		return ret;

	if (mode > 1) {
		pr_err("Invalid charge mode\n");
		return -EINVAL;
	}

	// Only bit 2 affects the charging mode, so shift 2 bits to the left.
	ret = gigabyte_laptop_set_devstate(CHARGING_MODE, mode << 2, &output);
	if (ret)
		return ret;

	gigabyte = dev_get_drvdata(dev);
	gigabyte->charge_mode = mode;
	return count;
}

/*
 * Maximum charge limit. Only works if custom charge mode is enabled.
 * Can be set between 60 and 100 percent.
 */
static ssize_t charge_limit_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->charge_limit);
}

static ssize_t charge_limit_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, output;
	unsigned int limit;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtouint(buf, 0, &limit);
	if (ret)
		return ret;

	if (limit > 100 || limit < 60) {
		pr_err("Invalid charge limit\n");
		return -EINVAL;
	}

	ret = gigabyte_laptop_set_devstate(CHARGING_LIMIT, limit, &output);
	if (ret)
		return ret;

	gigabyte = dev_get_drvdata(dev);
	gigabyte->charge_limit = limit;
	return count;
}

static ssize_t gpu_boost_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->gpu_boost);
}

static ssize_t gpu_boost_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, output;
	unsigned int mode;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtouint(buf, 0, &mode);
	if (ret)
		return ret;

	gigabyte = dev_get_drvdata(dev);

	/*
	 * On Gigabyte Gaming models (e.g. A16 CWH), 0x51 only supports 0/1
	 * (CTGP boost off/on). Values 3 and 4 send eject/power-off requests
	 * to the dGPU, so they must never be passed through.
	 */
	if ((gigabyte->gaming_family && mode > 1) || mode > 3) {
		pr_err("Invalid boost mode");
		return -EINVAL;
	}

	ret = gigabyte_laptop_set_devstate(GPU_QBOOST, mode, &output);
	if (ret)
		return ret;

	gigabyte->gpu_boost = mode;
	return count;
}

/*
 * Performance mode (Gigabyte Gaming models, e.g. A16 CWH).
 * Mirrors the Windows Control Center modes by programming the NVIDIA
 * platform controller (NPCF) through WMBD 0xED:
 * 0 = eco      (GPU TGP capped low)
 * 1 = balanced (default-ish limits)
 * 2 = boost    (max TGP + CTGP boost; on the A16 CWH this lifts the
 *               RTX 5070's cap from 55W toward 85W)
 */
static ssize_t perf_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->perf_mode);
}

static ssize_t perf_mode_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, output;
	unsigned int mode;
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	ret = kstrtouint(buf, 0, &mode);
	if (ret)
		return ret;

	if (mode > 2) {
		pr_err("Invalid performance mode\n");
		return -EINVAL;
	}

	ret = gigabyte_laptop_set_devstate(PERF_MODE, mode, &output);
	if (ret)
		return ret;

	gigabyte->perf_mode = mode;
	return count;
}

/*
 * GPU dynamic boost (Gigabyte Gaming models, e.g. A16 CWH).
 * Lets the NVIDIA driver shift extra power to the GPU under load
 * (up to Max Power Limit, e.g. 85W on the A16 CWH's RTX 5070).
 * 0 = off, 1 = on. The firmware argument is inverted (0 enables DBAC),
 * so flip it here to expose a conventional boolean.
 */
static ssize_t dynamic_boost_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret, output;

	ret = gigabyte_laptop_get_devstate(DYN_BOOST, &output); // returns DBAC
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d\n", output);
}

static ssize_t dynamic_boost_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, output;
	unsigned int enable;

	ret = kstrtouint(buf, 0, &enable);
	if (ret)
		return ret;

	if (enable > 1) {
		pr_err("Invalid dynamic boost value\n");
		return -EINVAL;
	}

	ret = gigabyte_laptop_set_devstate(DYN_BOOST, !enable, &output);
	if (ret)
		return ret;
	return count;
}

static ssize_t fan_turbo_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t fan_turbo_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count);
static ssize_t fan_gaming_boost_show(struct device *dev,
				     struct device_attribute *attr, char *buf);
static ssize_t fan_gaming_boost_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count);
static void gigabyte_fan_gaming_boost_stop(struct gigabyte_laptop_wmi *gigabyte,
					   struct device *dev);

static ssize_t fan_curve_index_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->fan_curve_index);
}

static ssize_t fan_curve_index_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	unsigned int index;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtouint(buf, 0, &index);
	if (ret)
		return ret;

	if (index >= FAN_CURVE_POINTS) {
		pr_err("Invalid fan curve index\n");
		return -EINVAL;
	}

	gigabyte = dev_get_drvdata(dev);
	gigabyte->fan_curve_index = index;
	return count;
}

static ssize_t fan_curve_data_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);
	int index = gigabyte->fan_curve_index;

	return sysfs_emit(buf, "%d %d\n", gigabyte->fan_curve.temperature[index],
		gigabyte->fan_curve.speed[index]);
}

static ssize_t fan_curve_data_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret, output;
	u16 data;
	u32 payload;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtou16(buf, 0, &data);
	if (ret)
		return ret;

	gigabyte = dev_get_drvdata(dev);
	// likely payload: speed, temp, index
	//payload = gigabyte->fan_curve.speed[gigabyte->fan_curve_index] << 16 | gigabyte->fan_curve.temperature[gigabyte->fan_curve_index] << 8 | (u8) gigabyte->fan_curve_index;
	payload = data << 8 | gigabyte->fan_curve_index;

	ret = gigabyte_laptop_set_devstate(FAN_INDEX_VALUE, payload, &output);
	if (ret)
		return ret;

	gigabyte->fan_curve.temperature[gigabyte->fan_curve_index] = data;
	gigabyte->fan_curve.speed[gigabyte->fan_curve_index] = data >> 8;
	return count;
}

static ssize_t battery_cycle_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret, cyc1, cyc2;

	ret = gigabyte_laptop_get_devstate(BATT_CYCLE, &cyc1);
	if (ret)
		return ret;
	ret = gigabyte_laptop_get_devstate(BATT_CYCLE2, &cyc2);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%d\n", max(cyc1, cyc2));
}

static ssize_t debug_method_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	u8 data;
	struct gigabyte_laptop_wmi *gigabyte;

	ret = kstrtou8(buf, 0, &data);
	if (ret)
		return ret;

	gigabyte = dev_get_drvdata(dev);
	gigabyte->debug_method = data;
	return count;
}

static ssize_t debug_method_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int ret, output;
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	ret = gigabyte_laptop_get_devstate(gigabyte->debug_method, &output);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%d, %d\n", gigabyte->debug_method, output);
}

#define TOGGLE_DEVICE(_device, _id) \
static ssize_t _device##_toggle_show(struct device *dev, struct device_attribute *attr, char *buf) \
{ \
	int ret, output; \
	ret = gigabyte_laptop_get_devstate(_id, &output); \
	if (ret) \
		return ret; \
	return sysfs_emit(buf, "%d\n", output); \
} \
static DEVICE_ATTR_RO(_device##_toggle);
TOGGLE_DEVICE(usb_charge_s3, USB_SLEEP);
TOGGLE_DEVICE(usb_charge_s4, USB_HIBERNATE);

static DEVICE_ATTR_RW(fan_mode);
static DEVICE_ATTR_RW(fan_custom_speed);
static DEVICE_ATTR_RW(charge_mode);
static DEVICE_ATTR_RW(charge_limit);
static DEVICE_ATTR_RW(gpu_boost);
static DEVICE_ATTR_RW(perf_mode);
static DEVICE_ATTR_RW(fan_turbo);
static DEVICE_ATTR_RW(fan_gaming_boost);
static DEVICE_ATTR_RW(dynamic_boost);
static DEVICE_ATTR_RW(fan_curve_index);
static DEVICE_ATTR_RW(fan_curve_data);
static DEVICE_ATTR_RO(battery_cycle);
static DEVICE_ATTR_RW(debug_method);

static int gigabyte_sysfs_usermode_cmd(char *cmd)
{
	char *argv[] = { "/bin/sh", "-c", cmd, NULL };
	char *envp[] = { "HOME=/", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", NULL };

	return call_usermodehelper(argv[0], argv, envp, UMH_WAIT_PROC);
}

static const int fan_gaming_boost_on_temp[] = { 0, 50, 58, 65, 72 };
static const int fan_gaming_boost_off_temp[] = { 0, 47, 55, 62, 69 };
static const int fan_gaming_boost_duty[] = { 0, 70, 80, 90, 100 };
static const int fan_gaming_boost_rpm_floor[] = { 0, 4200, 5000, 5700, 6200 };

static int gigabyte_fan_max_rpm(void)
{
	int r1, r2, ret;

	ret = gigabyte_laptop_get_devstate(FAN_CPU_RPM, &r1);
	if (ret)
		return 0;
	ret = gigabyte_laptop_get_devstate(FAN_GPU_RPM, &r2);
	if (ret)
		return r1;
	return max(r1, r2);
}

static int fan_gaming_boost_compute_level(int cur_level, int gpu_temp, int max_rpm)
{
	int temp_level, rpm_level, level;

	if (gpu_temp < FAN_GAMING_BOOST_IDLE_TEMP)
		return 0;

	temp_level = cur_level;
	while (temp_level > 0 && gpu_temp < fan_gaming_boost_off_temp[temp_level])
		temp_level--;
	while (temp_level < FAN_GAMING_BOOST_LEVELS &&
	       gpu_temp >= fan_gaming_boost_on_temp[temp_level + 1])
		temp_level++;

	rpm_level = 0;
	while (rpm_level < FAN_GAMING_BOOST_LEVELS &&
	       max_rpm < fan_gaming_boost_rpm_floor[rpm_level + 1])
		rpm_level++;

	level = max(temp_level, rpm_level);
	return max(level, 1);
}

static bool gigabyte_fan_gaming_boost_hw_ok(int level)
{
	int tenf, duty, ret;
	u8 expected;

	if (level == 0)
		return true;

	ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_MODE, &tenf);
	if (ret || !tenf)
		return false;

	expected = gigabyte_fan_display_to_duty(fan_gaming_boost_duty[level]);
	ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_SPEED, &duty);
	return !ret && duty == expected;
}

static int gigabyte_fan_gaming_boost_apply_usermode(struct device *dev, int duty)
{
	char *kpath, *cmd;
	int ret;

	kpath = kobject_get_path(&dev->kobj, GFP_KERNEL);
	if (!kpath)
		return -ENOMEM;

	cmd = kasprintf(GFP_KERNEL,
			"echo 3 > /sys%s/fan_mode && echo %d > /sys%s/fan_custom_speed",
			kpath, duty, kpath);
	kfree(kpath);
	if (!cmd)
		return -ENOMEM;

	ret = gigabyte_sysfs_usermode_cmd(cmd);
	kfree(cmd);
	return ret;
}

static int fan_gaming_boost_apply_level(struct device *dev,
					struct gigabyte_laptop_wmi *gigabyte,
					int level)
{
	char speed_buf[8];
	int duty, ret;
	ssize_t sret;

	if (level == 0) {
		sret = fan_mode_store(dev, &dev_attr_fan_mode, "2\n", 2);
		if (sret < 0)
			return (int)sret;
		gigabyte->fan_gaming_boost_active = 0;
		gigabyte->fan_gaming_boost_level = 0;
		return 0;
	}

	duty = fan_gaming_boost_duty[level];
	sret = fan_mode_store(dev, &dev_attr_fan_mode, "3\n", 2);
	if (sret < 0)
		return (int)sret;
	msleep(100);

	snprintf(speed_buf, sizeof(speed_buf), "%d\n", duty);
	sret = fan_custom_speed_store(dev, &dev_attr_fan_custom_speed,
				      speed_buf, strlen(speed_buf));
	if (sret < 0)
		return (int)sret;
	msleep(100);

	if (!gigabyte_fan_gaming_boost_hw_ok(level)) {
		ret = gigabyte_fan_gaming_boost_apply_usermode(dev, duty);
		if (ret)
			return ret;
		msleep(100);
	}

	if (!gigabyte_fan_gaming_boost_hw_ok(level))
		return -EIO;

	gigabyte->fan_gaming_boost_active = 1;
	gigabyte->fan_gaming_boost_level = level;
	return 0;
}

static void gigabyte_fan_gaming_boost_stop(struct gigabyte_laptop_wmi *gigabyte,
					   struct device *dev)
{
	cancel_delayed_work_sync(&gigabyte->fan_gaming_boost_work);
	gigabyte->fan_gaming_boost_enabled = 0;
	if (gigabyte->fan_gaming_boost_active)
		fan_gaming_boost_apply_level(dev, gigabyte, 0);
	gigabyte->fan_gaming_boost_level = 0;
}

static void gigabyte_fan_gaming_boost_work_fn(struct work_struct *work)
{
	struct gigabyte_laptop_wmi *gigabyte =
		container_of(to_delayed_work(work),
			     struct gigabyte_laptop_wmi, fan_gaming_boost_work);
	struct device *dev = gigabyte->fan_gaming_boost_dev;
	int gpu_temp, max_rpm, target_level, ret;
	bool need_apply;

	if (!gigabyte->fan_gaming_boost_enabled)
		return;

	if (gigabyte->fan_turbo_active) {
		schedule_delayed_work(&gigabyte->fan_gaming_boost_work,
			msecs_to_jiffies(FAN_GAMING_BOOST_POLL_MS));
		return;
	}

	ret = gigabyte_laptop_get_devstate(TEMP_GPU, &gpu_temp);
	if (ret)
		goto reschedule;

	max_rpm = gigabyte_fan_max_rpm();
	target_level = fan_gaming_boost_compute_level(
		gigabyte->fan_gaming_boost_level, gpu_temp, max_rpm);

	need_apply = target_level != gigabyte->fan_gaming_boost_level ||
		     (target_level > 0 &&
		      !gigabyte_fan_gaming_boost_hw_ok(target_level));

	if (need_apply) {
		ret = fan_gaming_boost_apply_level(dev, gigabyte, target_level);
		if (ret)
			pr_warn("gaming boost: apply level %d failed (%d)\n",
				target_level, ret);
		else
			pr_info("gaming boost: GPU %d°C RPM %d -> level %d (%d%%)\n",
				gpu_temp, max_rpm, target_level,
				fan_gaming_boost_duty[target_level]);
	}

reschedule:
	if (gigabyte->fan_gaming_boost_enabled)
		schedule_delayed_work(&gigabyte->fan_gaming_boost_work,
			msecs_to_jiffies(FAN_GAMING_BOOST_POLL_MS));
}

static ssize_t fan_gaming_boost_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", gigabyte->fan_gaming_boost_enabled);
}

static ssize_t fan_gaming_boost_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf, size_t count)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);
	unsigned int enable;
	int ret;

	ret = kstrtouint(buf, 0, &enable);
	if (ret)
		return ret;

	if (enable > 1) {
		pr_err("Invalid gaming boost value\n");
		return -EINVAL;
	}

	if (enable) {
		if (gigabyte->fan_gaming_boost_enabled)
			return count;

		if (gigabyte->fan_turbo_active) {
			pr_err("Disable fan_turbo before enabling gaming boost\n");
			return -EBUSY;
		}

		if (gigabyte->fan_mode != 2) {
			pr_err("fan_gaming_boost requires fan_mode=2 (gaming)\n");
			return -EINVAL;
		}

		gigabyte->fan_gaming_boost_enabled = 1;
		gigabyte->fan_gaming_boost_dev = dev;
		gigabyte->fan_gaming_boost_level = 0;
		gigabyte->fan_gaming_boost_active = 0;
		schedule_delayed_work(&gigabyte->fan_gaming_boost_work, 0);
		pr_info("gaming boost: enabled\n");
	} else {
		if (!gigabyte->fan_gaming_boost_enabled)
			return count;

		gigabyte_fan_gaming_boost_stop(gigabyte, dev);
		pr_info("gaming boost: disabled\n");
	}

	return count;
}

static bool gigabyte_fan_turbo_hw_active(void)
{
	int tenf, duty, ret;

	ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_MODE, &tenf);
	if (ret || !tenf)
		return false;

	ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_SPEED, &duty);
	if (ret || duty != FAN_DUTY_MAX)
		return false;

	return true;
}

static int gigabyte_fan_turbo_apply_max(struct device *dev)
{
	ssize_t ret;

	ret = fan_mode_store(dev, &dev_attr_fan_mode, "3\n", 2);
	if (ret < 0)
		return (int)ret;

	msleep(100);

	ret = fan_custom_speed_store(dev, &dev_attr_fan_custom_speed,
				     "100\n", 4);
	if (ret < 0)
		return (int)ret;

	msleep(100);

	if (!gigabyte_fan_turbo_hw_active())
		return -EIO;

	return 0;
}

static int gigabyte_fan_turbo_apply_max_usermode(struct device *dev)
{
	char *kpath, *cmd;
	int ret;

	kpath = kobject_get_path(&dev->kobj, GFP_KERNEL);
	if (!kpath)
		return -ENOMEM;

	cmd = kasprintf(GFP_KERNEL,
			"echo 3 > /sys%s/fan_mode && echo 100 > /sys%s/fan_custom_speed",
			kpath, kpath);
	kfree(kpath);
	if (!cmd)
		return -ENOMEM;

	ret = gigabyte_sysfs_usermode_cmd(cmd);
	kfree(cmd);
	if (ret)
		return ret;

	msleep(100);

	if (!gigabyte_fan_turbo_hw_active())
		return -EIO;

	return 0;
}

static void gigabyte_fan_turbo_work_fn(struct work_struct *work)
{
	struct gigabyte_laptop_wmi *gigabyte =
		container_of(work, struct gigabyte_laptop_wmi, fan_turbo_work);
	struct device *dev = gigabyte->fan_turbo_dev;
	char mode_buf[8];
	int ret = 0, output;

	if (gigabyte->fan_turbo_work_enable) {
		if (gigabyte->fan_gaming_boost_enabled)
			gigabyte_fan_gaming_boost_stop(gigabyte, dev);

		gigabyte->fan_turbo_saved_mode = gigabyte->fan_mode;
		gigabyte->fan_turbo_saved_display_speed =
			gigabyte->fan_custom_display_speed;
		gigabyte->fan_turbo_saved_internal_speed =
			gigabyte->fan_custom_internal_speed;

		ret = gigabyte_fan_turbo_apply_max(dev);
		if (ret) {
			pr_info("turbo fan: sysfs store path failed (%d), trying shell\n",
				ret);
			ret = gigabyte_fan_turbo_apply_max_usermode(dev);
		}
		if (ret) {
			pr_warn("turbo fan: enable failed (%d)\n", ret);
			goto done;
		}

		gigabyte->fan_turbo_active = 1;
		pr_info("turbo fan: enabled\n");
	} else {
		if (!gigabyte->fan_turbo_active)
			goto done;

		gigabyte_laptop_set_devstate(FAN_TURBO, 0, &output);

		snprintf(mode_buf, sizeof(mode_buf), "%d\n",
			 gigabyte->fan_turbo_saved_mode);
		ret = fan_mode_store(dev, &dev_attr_fan_mode, mode_buf,
				     strlen(mode_buf));
		if (ret < 0) {
			pr_warn("turbo fan: restore fan_mode failed (%d)\n",
				(int)ret);
			goto done;
		}

		gigabyte->fan_turbo_active = 0;
		pr_info("turbo fan: disabled\n");
	}

done:
	gigabyte->fan_turbo_work_result = ret;
	complete(&gigabyte->fan_turbo_done);
}

/*
 * Turbo fan (Gigabyte Gaming models, e.g. A16 CWH).
 * Engages custom mode at 100% duty. TFAN is not written on enable because it
 * can latch while TENF is still off on the A16 CWH.
 */
static ssize_t fan_turbo_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	if (gigabyte->fan_turbo_active && !gigabyte_fan_turbo_hw_active())
		gigabyte->fan_turbo_active = 0;

	return sysfs_emit(buf, "%d\n", gigabyte->fan_turbo_active);
}

static ssize_t fan_turbo_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);
	unsigned int enable;
	int ret;

	ret = kstrtouint(buf, 0, &enable);
	if (ret)
		return ret;

	if (enable > 1) {
		pr_err("Invalid turbo fan value\n");
		return -EINVAL;
	}

	if (enable) {
		if (gigabyte->fan_turbo_active && gigabyte_fan_turbo_hw_active())
			return count;
		gigabyte->fan_turbo_active = 0;
	} else if (!gigabyte->fan_turbo_active) {
		return count;
	}

	gigabyte->fan_turbo_work_enable = enable;
	gigabyte->fan_turbo_dev = dev;
	reinit_completion(&gigabyte->fan_turbo_done);
	queue_work(system_unbound_wq, &gigabyte->fan_turbo_work);
	wait_for_completion(&gigabyte->fan_turbo_done);

	if (gigabyte->fan_turbo_work_result)
		return gigabyte->fan_turbo_work_result;
	return count;
}

static struct attribute *gigabyte_laptop_attributes[] = {
	&dev_attr_fan_mode.attr,
	&dev_attr_fan_custom_speed.attr,
	&dev_attr_charge_mode.attr,
	&dev_attr_charge_limit.attr,
	&dev_attr_usb_charge_s3_toggle.attr,
	&dev_attr_usb_charge_s4_toggle.attr,
	&dev_attr_gpu_boost.attr,
	&dev_attr_perf_mode.attr,
	&dev_attr_fan_turbo.attr,
	&dev_attr_fan_gaming_boost.attr,
	&dev_attr_dynamic_boost.attr,
	&dev_attr_fan_curve_index.attr,
	&dev_attr_fan_curve_data.attr,
	&dev_attr_battery_cycle.attr,
	&dev_attr_debug_method.attr,
	NULL
};

static umode_t gigabyte_laptop_sysfs_is_visible(struct kobject *kobj, struct attribute *attr, int idx)
{
	struct device *dev = kobj_to_dev(kobj);
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	// Gaming-only sysfs nodes (method IDs absent on Aero/AORUS firmware)
	if (attr == &dev_attr_perf_mode.attr || attr == &dev_attr_fan_turbo.attr ||
	    attr == &dev_attr_fan_gaming_boost.attr ||
	    attr == &dev_attr_dynamic_boost.attr)
		return gigabyte->gaming_family ? attr->mode : 0;

	return attr->mode;
}

static const struct attribute_group gigabyte_laptop_attr_group = {
	.is_visible = gigabyte_laptop_sysfs_is_visible,
	.attrs = gigabyte_laptop_attributes,
};

#define DMI_EXACT_MATCH_GIGABYTE_LAPTOP_FAMILY(name) \
	{ .matches = { \
		DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "GIGABYTE"), \
		DMI_EXACT_MATCH(DMI_PRODUCT_FAMILY, name), \
	}}

#define DMI_EXACT_MATCH_GIGABYTE_LEGACY_DEVICE(name) \
	{ .matches = { \
		DMI_EXACT_MATCH(DMI_BOARD_VENDOR, "GIGABYTE"), \
		DMI_EXACT_MATCH(DMI_PRODUCT_NAME, name), \
	}}

static const struct dmi_system_id gigabyte_laptop_known_working_platforms[] = {
	DMI_EXACT_MATCH_GIGABYTE_LAPTOP_FAMILY("AERO"),
	DMI_EXACT_MATCH_GIGABYTE_LAPTOP_FAMILY("AORUS"),
	DMI_EXACT_MATCH_GIGABYTE_LAPTOP_FAMILY("GIGABYTE GAMING"),
	DMI_EXACT_MATCH_GIGABYTE_LAPTOP_FAMILY("GIGABYTE AERO"), // 16X, why
	// For older Aero models
	DMI_EXACT_MATCH_GIGABYTE_LAPTOP_FAMILY("Intel"),
	DMI_EXACT_MATCH_GIGABYTE_LEGACY_DEVICE("Aero 14"),
	DMI_EXACT_MATCH_GIGABYTE_LEGACY_DEVICE("P64V6"),
	DMI_EXACT_MATCH_GIGABYTE_LEGACY_DEVICE("P64V7"),
	{ }
};

/* Driver init ********************************************/

static int probe_custom_fan_speed(int speed)
{
	if (speed == 0x39)
		return 25;
	else if (speed == 0x44)
		return 30;
	else if (speed == 0x50)
		return 35;
	else if (speed == 0x5B)
		return 40;
	else if (speed == 0x67)
		return 45;
	else if (speed == 0x72)
		return 50;
	else if (speed == 0x7D)
		return 55;
	else if (speed == 0x89)
		return 60;
	else if (speed == 0x94)
		return 65;
	else if (speed == 0xA0)
		return 70;
	else if (speed == 0xAB)
		return 75;
	else if (speed == 0xB7)
		return 80;
	else if (speed == 0xC2)
		return 85;
	else if (speed == 0xCE)
		return 90;
	else if (speed == 0xD9)
		return 95;
	else if (speed == 0xE5)
		return 100;
	else // For something like 0x5D, which is unknown
		return 40;
}

static int gigabyte_laptop_probe(struct device *dev)
{
	int ret, output;
	u8 result, result2;
	struct gigabyte_laptop_wmi *gigabyte = dev_get_drvdata(dev);

	// Older devices are using a different method ID for silent fan mode.
	// In that case, newer devices won't return anything when using that ID.
	// Gigabyte Gaming firmware (e.g. A16 CWH) implements neither: its WMBC
	// default case echoes the argument back, and its WMBD 0xFA is an empty
	// stub, so probe with a sentinel value to detect the echo.
	output = -1;
	ret = gigabyte_laptop_get_devstate2(FAN_SILENT_OLD, 0xFA51, &output);
	if (ret || output < 0 || output == 0xFA51) {
		pr_info("Newer model detected, using new silent fan mode ID");
		gigabyte->fan_silent_method = FAN_SILENT_MODE;
	}
	else { // 0 on older devices
		pr_info("Older model detected, using old ID");
		gigabyte->fan_silent_method = FAN_SILENT_OLD;
	}

	// Set silent fan mode ID.
	fan_modes[1] = gigabyte->fan_silent_method;

	// Get current fan mode.
	ret = gigabyte_laptop_get_devstate(gigabyte->fan_silent_method, &output);
	if (ret)
		return ret;
	else if (output) {
		gigabyte->fan_mode = 1;
		goto obtain_custom_fan_speed;
	}
	ret = gigabyte_laptop_get_devstate(FAN_GAMING_MODE, &output);
	if (ret)
		return ret;
	else if (output) {
		gigabyte->fan_mode = 2;
		goto obtain_custom_fan_speed;
	}
	ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_MODE, &output);
	if (ret)
		return ret;
	else if (output) {
		// Auto-maximum mode can't be read through WMI, so read EC register containing it
		ret = ec_read(0xD, &result);
		if (ret)
			return AE_ERROR;
		output = (result >> 7) & 0x1;
		if (output) {
			gigabyte->fan_mode = 4;
			goto obtain_custom_fan_speed;
		}
		ret = gigabyte_laptop_get_devstate(FAN_FIXED_MODE, &output);
		if (ret)
			return ret;
		else if (output)
			gigabyte->fan_mode = 5;
		else
			gigabyte->fan_mode = 3;
		goto obtain_custom_fan_speed;
	}
	// If all checks return 0, we are most likely in normal fan mode
	gigabyte->fan_mode = 0;

obtain_custom_fan_speed:
	ret = gigabyte_laptop_get_devstate(FAN_CUSTOM_SPEED, &output);
	if (ret)
		return ret;
	else if (output) {
		gigabyte->fan_custom_display_speed = probe_custom_fan_speed(output);
		gigabyte->fan_custom_internal_speed = output;
	}

	/*
		Some newer models don't change both fans' speed together through
		FAN_CUSTOM_SPEED. If this is the case, we will have to modify FAN2
		directly using ec_write. Doing it through WMI would be better if
		we did not also have to modify GFTY as well, which already changes
		on its own without us doing anything.
	*/
	ret = gigabyte_laptop_set_devstate(FAN_CUSTOM_SPEED, 255, &output);
	ec_read(0xB0, &result);
	ec_read(0xB1, &result2);
	if (result != result2) {
		pr_info("Dual fan speed control required\n");
		gigabyte->dual_fan_speed_enabled = 1;
	}
	ret = gigabyte_laptop_set_devstate(FAN_CUSTOM_SPEED,
		gigabyte->fan_custom_internal_speed, &output);

	ret = gigabyte_laptop_get_devstate(CHARGING_MODE, &output);
	if (ret)
		return ret;
	else if (output)
		gigabyte->charge_mode = output >> 2;

	ret = gigabyte_laptop_get_devstate(CHARGING_LIMIT, &output);
	if (ret)
		return ret;
	else if (output)
		gigabyte->charge_limit = output;

	// Get the fan curve. Used by custom mode.
	for (u8 i = 0; i < FAN_CURVE_POINTS; i++) {
		ret = gigabyte_laptop_get_devstate2(FAN_INDEX_VALUE, i, &output);
		if (ret)
			return ret;
		else if (output) {
			gigabyte->fan_curve.temperature[i] = output;
			gigabyte->fan_curve.speed[i] = output >> 8;
		}
	}

	return 0;
}

static struct platform_driver platform_driver = {
	.driver = {
		.name = GIGABYTE_LAPTOP_FILE,
		.owner = THIS_MODULE,
	},
};

static void __exit gigabyte_laptop_exit(void)
{
	struct gigabyte_laptop_wmi *gigabyte;

	pr_info("Goodbye, World!\n");
	gigabyte = platform_get_drvdata(platform_device);
	cancel_delayed_work_sync(&gigabyte->fan_gaming_boost_work);
	cancel_work_sync(&gigabyte->fan_turbo_work);
	hwmon_device_unregister(gigabyte->hwmon_dev);
	sysfs_remove_group(&gigabyte->pdev->dev.kobj, &gigabyte_laptop_attr_group);
	platform_driver_unregister(&platform_driver);
	platform_device_unregister(gigabyte->pdev);
	kfree(gigabyte);
}

static int __init gigabyte_laptop_init(void)
{
	struct gigabyte_laptop_wmi *gigabyte;
	int result;

	if (!wmi_has_guid(WMI_METHOD_WMBC) ||
		!wmi_has_guid(WMI_METHOD_WMBD)) {
		pr_warn("No known WMI GUID found!\n");
		return -ENODEV;
	}

	if (!dmi_check_system(gigabyte_laptop_known_working_platforms)) {
		pr_err("Laptop not supported\n");
		return -ENODEV;
	}

	result = platform_driver_register(&platform_driver);
	if (result) {
		pr_warn("Unable to register platform driver\n");
		return result;
	}

	gigabyte = kzalloc(sizeof(struct gigabyte_laptop_wmi), GFP_KERNEL);
	if (!gigabyte) {
		result = -ENOMEM;
		goto fail_platform_driver;
	}

	platform_device = platform_device_alloc(GIGABYTE_LAPTOP_FILE, -1);
	if (!platform_device) {
		pr_warn("Unable to allocate platform device\n");
		kfree(gigabyte);
		result = -ENOMEM;
		goto fail_platform_driver;
	}

	gigabyte->pdev = platform_device;
	if (!strcmp(dmi_get_system_info(DMI_PRODUCT_FAMILY), "GIGABYTE GAMING"))
		gigabyte->gaming_family = 1;
	INIT_WORK(&gigabyte->fan_turbo_work, gigabyte_fan_turbo_work_fn);
	init_completion(&gigabyte->fan_turbo_done);
	INIT_DELAYED_WORK(&gigabyte->fan_gaming_boost_work,
			  gigabyte_fan_gaming_boost_work_fn);
	platform_set_drvdata(gigabyte->pdev, gigabyte);

	result = platform_device_add(gigabyte->pdev);
	if (result) {
		pr_warn("Unable to add platform device\n");
		goto fail_platform_device;
	}

	result = sysfs_create_group(&gigabyte->pdev->dev.kobj,
					&gigabyte_laptop_attr_group);
	if (result)
		goto fail_sysfs;

	gigabyte->hwmon_dev = hwmon_device_register_with_info(&gigabyte->pdev->dev,
			GIGABYTE_LAPTOP_FILE, gigabyte, &gigabyte_laptop_chip_info, NULL);
	if (IS_ERR(gigabyte->hwmon_dev)) {
		result = PTR_ERR(gigabyte->hwmon_dev);
		pr_err("hwmon registration failed with %d\n", result);
		goto fail_sysfs;
	}

	result = gigabyte_laptop_probe(&gigabyte->pdev->dev);
	if (result) {
		pr_err("Probe failed\n");
		goto fail_probe;
	}
	pr_info("Hello, World!\n");
	return 0;

fail_probe:
	hwmon_device_unregister(gigabyte->hwmon_dev);
	sysfs_remove_group(&gigabyte->pdev->dev.kobj, &gigabyte_laptop_attr_group);
fail_sysfs:
	platform_device_del(gigabyte->pdev);
fail_platform_device:
	platform_device_put(gigabyte->pdev);
fail_platform_driver:
	platform_driver_unregister(&platform_driver);
	return result;
}

module_init(gigabyte_laptop_init);
module_exit(gigabyte_laptop_exit);
