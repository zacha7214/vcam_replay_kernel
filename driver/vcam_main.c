// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_replay - module entry point.
 *
 * On load:
 *   - creates `devices` chardev-fed V4L2 instances (Phase 1: frames come
 *     from userspace via /dev/vcamN, replayed at `fps` by an hrtimer)
 *   - registers the USB driver (Phase 2: instances appear/disappear as
 *     "usb-vcam" QEMU devices are plugged/unplugged)
 */

#include <linux/module.h>
#include <linux/videodev2.h>

#include "vcam.h"

#define VCAM_MAX_DEVICES 8

static unsigned int devices = 1;
module_param(devices, uint, 0444);
MODULE_PARM_DESC(devices, "number of chardev-fed instances (default 1)");

static unsigned int width = 640;
module_param(width, uint, 0444);
MODULE_PARM_DESC(width, "initial frame width (default 640)");

static unsigned int height = 480;
module_param(height, uint, 0444);
MODULE_PARM_DESC(height, "initial frame height (default 480)");

static char *format = "GREY";
module_param(format, charp, 0444);
MODULE_PARM_DESC(format, "initial pixel format: GREY or YUYV");

static unsigned int fps = 30;
module_param(fps, uint, 0444);
MODULE_PARM_DESC(fps, "initial playback rate in frames/second (default 30)");

static struct vcam_chardev *instances[VCAM_MAX_DEVICES];
static unsigned int num_instances;

static int __init vcam_init(void)
{
	struct vcam_config cfg = {};
	char name[24];
	unsigned int i;
	int ret;

	if (!strcasecmp(format, "YUYV"))
		cfg.fourcc = V4L2_PIX_FMT_YUYV;
	else if (!strcasecmp(format, "GREY"))
		cfg.fourcc = V4L2_PIX_FMT_GREY;
	else {
		pr_err("%s: unknown format '%s' (use GREY or YUYV)\n",
		       VCAM_MODULE_NAME, format);
		return -EINVAL;
	}

	cfg.width = width;
	cfg.height = height;
	cfg.fps_num = fps;
	cfg.fps_den = 1;
	cfg.self_paced = true;
	cfg.name = name;

	for (i = 0; i < min_t(unsigned int, devices, VCAM_MAX_DEVICES); i++) {
		snprintf(name, sizeof(name), "vcam%u", i);
		instances[i] = vcam_chardev_create(&cfg);
		if (IS_ERR(instances[i])) {
			ret = PTR_ERR(instances[i]);
			goto err_instances;
		}
		num_instances++;
	}

	ret = vcam_usb_register();
	if (ret)
		goto err_instances;
	return 0;

err_instances:
	while (num_instances)
		vcam_chardev_destroy(instances[--num_instances]);
	return ret;
}

static void __exit vcam_exit(void)
{
	vcam_usb_unregister();
	while (num_instances)
		vcam_chardev_destroy(instances[--num_instances]);
}

module_init(vcam_init);
module_exit(vcam_exit);

MODULE_AUTHOR("Zach Allen");
MODULE_DESCRIPTION("Simulated USB camera video replay (V4L2 + QEMU USB)");
MODULE_LICENSE("GPL v2");
