/*
 * lge/com_device/input/sweep2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 * Copyright (c) 2015, Vineeth Raj <contact.twn@openmailbox.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/input/sweep2wake.h>
#include <linux/input/wake_helpers.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/hrtimer.h>

#define SUPPORT_FOR_LG_LTE
/* uncomment since no touchscreen defines android touch, do that here */
//#define ANDROID_TOUCH_DECLARED

/* uncomment if s2w_scr_suspended is updated automagically */
//#define WAKE_HOOKS_DEFINED

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
#include <linux/lcd_notify.h>
#else
#include <linux/earlysuspend.h>
#endif
#endif //WAKE_HOOKS_DEFINED

/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Dennis Rassmann <showp1984@gmail.com>"
#define DRIVER_DESCRIPTION "Sweep2wake for almost any device"
#define DRIVER_VERSION "1.5.7"
#define LOGTAG "[sweep2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define S2W_DEBUG				0
#define S2W_DEFAULT				0
#define S2W_S2SONLY_DEFAULT		0
#define S2W_PWRKEY_DUR			60
#define S2W_FEATHER				200

#ifdef CONFIG_MACH_MSM8974_HAMMERHEAD
/* Hammerhead aka Nexus 5 */
#define S2W_Y_MAX				1920
#define S2W_X_MAX				1080
#define S2W_Y_LIMIT				S2W_Y_MAX-130
#define S2W_X_B1				400
#define S2W_X_B2				700
#define S2W_X_FINAL				250
#elif defined(CONFIG_MACH_APQ8064_MAKO)
/* Mako aka Nexus 4 */
#define S2W_Y_LIMIT				2350
#define S2W_X_MAX				1540
#define S2W_X_B1				500
#define S2W_X_B2				1000
#define S2W_X_FINAL				300
#elif defined(CONFIG_MACH_APQ8064_FLO)
/* Flo/Deb aka Nexus 7 2013 */
#define S2W_Y_MAX				2240
#define S2W_X_MAX				1344
#define S2W_Y_LIMIT				S2W_Y_MAX-110
#define S2W_X_B1				500
#define S2W_X_B2				700
#define S2W_X_FINAL				450
#elif defined(CONFIG_ARCH_MSM8226)
/* LG LU6200/SU640 */
#define S2W_Y_MAX				1350
#define S2W_X_MAX				720
#ifdef SUPPORT_FOR_LG_LTE
#define S2W_Y_LIMIT				1280
#else
#define S2W_Y_LIMIT				1180
#endif
#define S2W_X_B1				155
#define S2W_X_B2				355
#define S2W_X_FINAL				175
#ifdef SUPPORT_FOR_LG_LTE
#define LG_SUPPORT_S2W
#endif
#define S2W_KEY_LEFT			160
#define S2W_KEY_CENTER			360
#define S2W_KEY_RIGHT			570
#define S2W_Y_B1				300
#define S2W_Y_B2				S2W_Y_LIMIT-300
#elif defined(CONFIG_MACH_PICO)
/* HTC Pico 2011 */
#define S2W_Y_MAX				1050
#define S2W_X_MAX				1024
#define S2W_Y_LIMIT				910
#define S2W_X_B1				256
#define S2W_X_B2				512
#define S2W_X_FINAL				768
#define S2W_Y_B1				300
#define S2W_Y_B2				S2W_Y_LIMIT-300
#else
/* defaults */
#define S2W_Y_LIMIT				2350
#define S2W_X_MAX				1540
#define S2W_X_B1				500
#define S2W_X_B2				1000
#define S2W_X_FINAL				300
#endif

/* Resources */
int s2w_switch = S2W_DEFAULT, s2w_s2sonly = S2W_S2SONLY_DEFAULT;
static int touch_x = 0, touch_y = 0;
static int y_init = 0;
#ifdef LG_SUPPORT_S2W
static int x_pre = 0;
int s2w_keypad_swipe_length = 3;
#endif
static bool touch_x_called = false, touch_y_called = false;
static bool exec_count = true;
bool s2w_scr_suspended = false;
static bool scr_on_touch = false, barrier[2] = {false, false};
static int key_code = KEY_POWER;
static bool is_ltr = false;
static bool is_ltr_set = false;
#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
static struct notifier_block s2w_lcd_notif;
#endif
#endif // WAKE_HOOKS_DEFINED
static struct input_dev * sweep2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static struct workqueue_struct *s2w_input_wq;
static struct work_struct s2w_input_work;

/* Read cmdline for s2w */
static int __init read_s2w_cmdline(char *s2w)
{
	if (strcmp(s2w, "1") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake enabled. | s2w='%s'\n", s2w);
		s2w_switch = 1;
	} else if (strcmp(s2w, "2") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake full screen enabled. | s2w='%s'\n", s2w);
		s2w_switch = 2;
	} else if (strcmp(s2w, "3") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake MusiqMod enabled. | s2w='%s'\n", s2w);
		s2w_switch = 3;
	} else if (strcmp(s2w, "0") == 0) {
		pr_info("[cmdline_s2w]: Sweep2Wake disabled. | s2w='%s'\n", s2w);
		s2w_switch = 0;
	} else {
		pr_info("[cmdline_s2w]: No valid input found. Going with default: | s2w='%u'\n", s2w_switch);
	}
	return 1;
}
__setup("s2w=", read_s2w_cmdline);

/* PowerKey work func */
static void sweep2wake_presspwr(struct work_struct * sweep2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
		return;
	input_event(sweep2wake_pwrdev, EV_KEY, key_code, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
	input_event(sweep2wake_pwrdev, EV_KEY, key_code, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(S2W_PWRKEY_DUR);
	mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

/* PowerKey trigger */
static void sweep2wake_pwrtrigger(void) {
	schedule_work(&sweep2wake_presspwr_work);
	return;
}

/* reset on finger release */
static void sweep2wake_reset(void) {
	exec_count = true;
	barrier[0] = false;
	barrier[1] = false;
	scr_on_touch = false;
	is_ltr = false;
	is_ltr_set = false;
#ifdef LG_SUPPORT_S2W
	x_pre = 0;
#endif
	y_init = 0;
	touch_x = touch_y = 0;
	key_code = KEY_POWER;
#if S2W_DEBUG
	pr_info(LOGTAG"sweep2wake_reset called!\n");
#endif
}

/* Sweep2wake main function */
static void detect_sweep2wake(int x, int y, bool st)
{
	int prevx = 0, nextx = 0;
	bool single_touch = st;
#if S2W_DEBUG
	pr_info(LOGTAG"x: %4d,x_pre: %4d\n",
			x, x_pre);
#endif
#ifdef LG_SUPPORT_S2W
	if (x_pre) {
		if (s2w_keypad_swipe_length == 2) {
			if (x == S2W_KEY_CENTER) {
				if (x_pre == S2W_KEY_LEFT) {
					if (s2w_scr_suspended) {
#if S2W_DEBUG
						pr_info(LOGTAG"LTR: keypad: ON\n");
#endif
						sweep2wake_pwrtrigger();
					}
				} else if (x_pre == S2W_KEY_RIGHT) {
					if (!s2w_scr_suspended) {
#if S2W_DEBUG
						pr_info(LOGTAG"RTL: keypad: OFF\n");
#endif
						sweep2wake_pwrtrigger();
					}
				}
			}
		} else if (s2w_keypad_swipe_length == 3) {
			if (x_pre == S2W_KEY_CENTER) {
				if (x == S2W_KEY_LEFT) {
					if (!s2w_scr_suspended) {
#if S2W_DEBUG
						pr_info(LOGTAG"RTL: keypad: OFF\n");
#endif
						sweep2wake_pwrtrigger();
					}
				} else if (x == S2W_KEY_RIGHT) {
					if (s2w_scr_suspended) {
#if S2W_DEBUG
						pr_info(LOGTAG"LTR: keypad: ON\n");
#endif
						sweep2wake_pwrtrigger();
					}
				}
			}
		}
		return;
	}
#endif // LG_SUPPORT_S2W

	if ((single_touch) && (s2w_scr_suspended == true) && (s2w_switch > 0 && ((s2w_switch == 3) ? 1 : !s2w_s2sonly))) {
		//left->right (screen_off)
		if (is_ltr) {
			prevx = 0;
			nextx = S2W_X_B1;
			if ((barrier[0] == true) ||
				((x > prevx) &&
				(x < nextx) &&
				(y > (((s2w_switch == 2) || ((s2w_switch == 3) ? (is_headset_in_use() || dt2w_sent_play_pause) : 0)) ? 0 : S2W_Y_LIMIT)))) {
				prevx = nextx;
				nextx = S2W_X_B2;
				barrier[0] = true;
				if ((barrier[1] == true) ||
					((x > prevx) &&
					(x < nextx) &&
					(y > (((s2w_switch == 2) || ((s2w_switch == 3) ? (is_headset_in_use() || dt2w_sent_play_pause) : 0)) ? 0 : S2W_Y_LIMIT)))) {
					prevx = nextx;
					barrier[1] = true;
					if ((x > prevx) &&
						(y > (((s2w_switch == 2) || ((s2w_switch == 3) ? (is_headset_in_use() || dt2w_sent_play_pause) : 0)) ? 0 : S2W_Y_LIMIT))) {
						if (x > (S2W_X_MAX - S2W_X_FINAL)) {
							if (exec_count) {
								if ((s2w_switch == 3) && (is_headset_in_use() || dt2w_sent_play_pause) && (y < S2W_Y_LIMIT)) {
									if (y <= S2W_Y_B1) {
										pr_info(LOGTAG"LTR: MusiqMod: volume up!\n");
										key_code = KEY_VOLUMEUP;
										sweep2wake_pwrtrigger();
									} else if (y >= S2W_Y_B2) {
										pr_info(LOGTAG"LTR: MusiqMod: next song\n");
										key_code = KEY_NEXTSONG;
										sweep2wake_pwrtrigger();
									} else {
										sweep2wake_reset();
									}
								} else {
									pr_info(LOGTAG"LTR: ON\n");
									key_code = KEY_POWER;
									sweep2wake_pwrtrigger();
								}
								exec_count = false;
							}
						}
					}
				}
			}
		//right->left (screen_off): handle MusiqMod(e)
		} else {
			prevx = S2W_X_MAX;
			nextx = S2W_X_B2;
			if ((barrier[0] == true) ||
				((x < prevx) &&
				(x > nextx) &&
				(y > (((s2w_switch == 2) || ((s2w_switch == 3) ? (is_headset_in_use() || dt2w_sent_play_pause) : 0)) ? 0 : S2W_Y_LIMIT)))) {
				prevx = nextx;
				nextx = S2W_X_B1;
				barrier[0] = true;
				if ((barrier[1] == true) ||
					((x < prevx) &&
					(x > nextx) &&
					(y > (((s2w_switch == 2) || ((s2w_switch == 3) ? (is_headset_in_use() || dt2w_sent_play_pause) : 0)) ? 0 : S2W_Y_LIMIT)))) {
					prevx = nextx;
					barrier[1] = true;
					if ((x < prevx) &&
						(y > (((s2w_switch == 2) || ((s2w_switch == 3) ? (is_headset_in_use() || dt2w_sent_play_pause) : 0)) ? 0 : S2W_Y_LIMIT))) {
						if (x < S2W_X_B1) {
							if (exec_count) {
								if ((s2w_switch == 3) && (is_headset_in_use() || dt2w_sent_play_pause) && (y < S2W_Y_LIMIT)) {
									pr_info(LOGTAG"LTR: MusiqMod: y = %i, S2W_Y_B2 = %i!\n", y, S2W_Y_B2);
									if (y <= S2W_Y_B1) {
										pr_info(LOGTAG"LTR: MusiqMod: volume down!\n");
										key_code = KEY_VOLUMEDOWN;
										sweep2wake_pwrtrigger();
									} else if (y >= S2W_Y_B2) {
										pr_info(LOGTAG"RTL: MusiqMod: previous song\n");
										key_code = KEY_PREVIOUSSONG;
										sweep2wake_pwrtrigger();
									} else {
										sweep2wake_reset();
									}
								} else {
									sweep2wake_reset();
								}
								exec_count = false;
							}
						}
					}
				}
			}
		}
	//right->left (screen_on)
	} else if ((single_touch) && (s2w_scr_suspended == false) && (s2w_switch > 0)) {
		scr_on_touch=true;
		prevx = S2W_X_MAX;
		nextx = S2W_X_B2;
		if ((barrier[0] == true) ||
			((x < prevx) &&
			(x > nextx) &&
			(y > S2W_Y_LIMIT))) {
			prevx = nextx;
			nextx = S2W_X_B1;
			barrier[0] = true;
			if ((barrier[1] == true) ||
				((x < prevx) &&
				(x > nextx) &&
				(y > S2W_Y_LIMIT))) {
				prevx = nextx;
				barrier[1] = true;
				if ((x < prevx) &&
					(y > S2W_Y_LIMIT)) {
					if (x < S2W_X_B1) {
						if (exec_count) {
							pr_info(LOGTAG"RTL: OFF\n");
							key_code = KEY_POWER;
							sweep2wake_pwrtrigger();
							exec_count = false;
						}
					}
				}
			}
		}
	}
}

static void s2w_input_callback(struct work_struct *unused) {
	if (is_earpiece_on()) {
#if S2W_DEBUG
		pr_info("Sweep2Wake: earpiece on! return!\n");
#endif
		return;
	}

	detect_sweep2wake(touch_x, touch_y, true);

	return;
}

static void s2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
#if S2W_DEBUG
	pr_info("sweep2wake: code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		(code==ABS_MT_TRACKING_ID) ? "ID" :
		"undef"), code, value);
#endif
	if (code == ABS_MT_SLOT) {
		sweep2wake_reset();
		return;
	}

	if (code == ABS_MT_TRACKING_ID && value == -1) {
#ifdef LG_SUPPORT_S2W
		if (x_pre == 0)
#endif
			sweep2wake_reset();
		return;
	}

	if (code == ABS_MT_POSITION_X) {
#ifdef LG_SUPPORT_S2W
		if ((value == S2W_KEY_LEFT) || (value == S2W_KEY_CENTER) || (value == S2W_KEY_RIGHT)) {
			if (x_pre == 0) {
				if ((value == S2W_KEY_LEFT) || (value == S2W_KEY_RIGHT)) {
					if (s2w_scr_suspended) {
						if (value == S2W_KEY_LEFT) {
							x_pre = value;
						}
					} else {
						if (value == S2W_KEY_RIGHT) {
							x_pre = value;
						}
					}
				}
			} else {
				if (s2w_keypad_swipe_length == 3) {
					if (value == S2W_KEY_CENTER)
						x_pre = value;

					if (x_pre == S2W_KEY_CENTER) {
						if (touch_x == S2W_KEY_LEFT)
							if (value == S2W_KEY_RIGHT)
								if (s2w_scr_suspended)
									sweep2wake_reset();

						if (touch_x == S2W_KEY_RIGHT)
							if (value == S2W_KEY_LEFT)
								if (!s2w_scr_suspended)
									sweep2wake_reset();
					}
				}
			}
		}
#endif // LG_SUPPORT_S2W
		touch_x = value;
		touch_x_called = true;
	}

	if (code == ABS_MT_POSITION_Y) {
		if (!y_init) {
			y_init = value;
		} else {
			if (abs(value - y_init) > S2W_FEATHER) {
				pr_info(LOGTAG"y crossed S2W_FEATHER val of %i\n", S2W_FEATHER);
				sweep2wake_reset();
			}
		}
		touch_y = value;
#ifdef LG_SUPPORT_S2W
		if (x_pre) {
			if (value < S2W_Y_LIMIT) {
				x_pre = 0;
			}
		}
#endif
		touch_y_called = true;
	}


#ifdef LG_SUPPORT_S2W
	if (touch_x_called && (x_pre ? true : (touch_y_called ? true : false))) {
#else
	if (touch_x_called && touch_y_called) {
#endif // LG_SUPPORT_S2W
		touch_x_called = false;
		touch_y_called = false;
		if (!is_ltr_set) {
			is_ltr_set = true;
			is_ltr = (touch_x > S2W_X_B2) ? false : true;
		}
		queue_work_on(0, s2w_input_wq, &s2w_input_work);
	}
}

static int input_dev_filter(struct input_dev *dev) {
	if (strstr(dev->name, "ft5x06")
		||strstr(dev->name, "ist30xx_ts")
		||strstr(dev->name, "Goodix-CTP")
		||strstr(dev->name, "himax-touchscreen")
		) {
		return 0;
	} else {
		return 1;
	}
}

static int s2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "s2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void s2w_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id s2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler s2w_input_handler = {
	.event		= s2w_input_event,
	.connect	= s2w_input_connect,
	.disconnect	= s2w_input_disconnect,
	.name		= "s2w_inputreq",
	.id_table	= s2w_ids,
};

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
	case LCD_EVENT_ON_END:
		s2w_scr_suspended = false;
		sweep2wake_reset();
		break;
	case LCD_EVENT_OFF_END:
		s2w_scr_suspended = true;
		sweep2wake_reset();
		break;
	default:
		break;
	}

	return 0;
}
#else
static void s2w_early_suspend(struct early_suspend *h) {
	s2w_scr_suspended = true;
}

static void s2w_late_resume(struct early_suspend *h) {
	s2w_scr_suspended = false;
}

static struct early_suspend s2w_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = s2w_early_suspend,
	.resume = s2w_late_resume,
};
#endif
#endif // WAKE_HOOKS_DEFINED

/*
 * SYSFS stuff below here
 */
static ssize_t s2w_sweep2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_switch);

	return count;
}

static ssize_t s2w_sweep2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '3' && buf[1] == '\n')
		if (s2w_switch != buf[0] - '0')
			s2w_switch = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(sweep2wake, (S_IWUSR|S_IRUGO),
	s2w_sweep2wake_show, s2w_sweep2wake_dump);

#ifdef LG_SUPPORT_S2W
static ssize_t s2w_sweep2wake_distance_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_keypad_swipe_length);

	return count;
}

static ssize_t s2w_sweep2wake_distance_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '1' && buf[0] <= '4' && buf[1] == '\n')
		if (s2w_keypad_swipe_length != buf[0] - '0')
			s2w_keypad_swipe_length = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(sweep2wake_distance, (S_IWUSR|S_IRUGO),
	s2w_sweep2wake_distance_show, s2w_sweep2wake_distance_dump);
#endif // LG_SUPPORT_S2W

static ssize_t s2w_s2w_s2sonly_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_s2sonly);

	return count;
}

static ssize_t s2w_s2w_s2sonly_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '1' && buf[1] == '\n')
		if (s2w_s2sonly != buf[0] - '0')
			s2w_s2sonly = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(s2w_s2sonly, (S_IWUSR|S_IRUGO),
	s2w_s2w_s2sonly_show, s2w_s2w_s2sonly_dump);

static ssize_t s2w_version_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%s\n", DRIVER_VERSION);

	return count;
}

static ssize_t s2w_version_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return count;
}

static DEVICE_ATTR(sweep2wake_version, (S_IWUSR|S_IRUGO),
	s2w_version_show, s2w_version_dump);

/*
 * INIT / EXIT stuff below here
 */
#ifdef ANDROID_TOUCH_DECLARED
extern struct kobject *android_touch_kobj;
#else
struct kobject *android_touch_kobj;
EXPORT_SYMBOL_GPL(android_touch_kobj);
#endif
static int __init sweep2wake_init(void)
{
	int rc = 0;

	sweep2wake_pwrdev = input_allocate_device();
	if (!sweep2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_POWER);
	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_NEXTSONG);
	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_PREVIOUSSONG);
	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_VOLUMEDOWN);
	input_set_capability(sweep2wake_pwrdev, EV_KEY, KEY_VOLUMEUP);
	sweep2wake_pwrdev->name = "s2w_pwrkey";
	sweep2wake_pwrdev->phys = "s2w_pwrkey/input0";

	rc = input_register_device(sweep2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}

	s2w_input_wq = create_workqueue("s2wiwq");
	if (!s2w_input_wq) {
		pr_err("%s: Failed to create s2wiwq workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&s2w_input_work, s2w_input_callback);
	rc = input_register_handler(&s2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register s2w_input_handler\n", __func__);

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
	s2w_lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&s2w_lcd_notif) != 0) {
		pr_err("%s: Failed to register lcd callback\n", __func__);
	}
#else
	register_early_suspend(&s2w_early_suspend_handler);
#endif
#endif // WAKE_HOOKS_DEFINED

#ifndef ANDROID_TOUCH_DECLARED
	android_touch_kobj = kobject_create_and_add("android_touch", NULL) ;
	if (android_touch_kobj == NULL) {
		pr_warn("%s: android_touch_kobj create_and_add failed\n", __func__);
	}
#endif
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sweep2wake\n", __func__);
	}
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_s2w_s2sonly.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for s2w_s2sonly\n", __func__);
	}
#ifdef LG_SUPPORT_S2W
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake_distance.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sweep2wake_distance\n", __func__);
	}
#endif // LG_SUPPORT_S2W
	rc = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake_version.attr);
	if (rc) {
		pr_warn("%s: sysfs_create_file failed for sweep2wake_version\n", __func__);
	}

err_input_dev:
	input_free_device(sweep2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);

	return 0;
}

static void __exit sweep2wake_exit(void)
{
#ifndef ANDROID_TOUCH_DECLARED
	kobject_del(android_touch_kobj);
#endif
#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
	lcd_unregister_client(&s2w_lcd_notif);
#endif
#endif // WAKE_HOOKS_DEFINED
	input_unregister_handler(&s2w_input_handler);
	destroy_workqueue(s2w_input_wq);
	input_unregister_device(sweep2wake_pwrdev);
	input_free_device(sweep2wake_pwrdev);
	return;
}

module_init(sweep2wake_init);
module_exit(sweep2wake_exit);
