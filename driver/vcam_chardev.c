// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_replay - character-device frontend (Phase 1).
 *
 * Creates a control node /dev/<name> (e.g. /dev/vcam0) next to the V4L2
 * video node. A userspace feeder (tools/vcam_feed) pushes raw frames into
 * the driver with plain write() calls — one full frame per call — and
 * configures format/rate via the VCAM_IOC_* ioctls (see vcam_uapi.h).
 * The core's hrtimer then replays the most recent frame to V4L2 consumers
 * at the configured fps.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>

#include "vcam.h"
#include "vcam_uapi.h"

struct vcam_chardev {
	struct miscdevice misc;
	struct vcam_dev *vcam;
	char name[24];
};

static struct vcam_chardev *file_to_cd(struct file *file)
{
	/* misc core points private_data at the miscdevice on open */
	return container_of(file->private_data, struct vcam_chardev, misc);
}

static ssize_t vcam_cdev_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct vcam_chardev *cd = file_to_cd(file);
	int ret;

	ret = vcam_submit_frame_user(cd->vcam, buf, count);
	if (ret)
		return ret;
	return count;
}

static long vcam_cdev_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct vcam_chardev *cd = file_to_cd(file);
	struct vcam_dev *vcam = cd->vcam;
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case VCAM_IOC_S_FORMAT: {
		struct vcam_format fmt;

		if (copy_from_user(&fmt, argp, sizeof(fmt)))
			return -EFAULT;
		return vcam_set_format(vcam, fmt.width, fmt.height,
				       fmt.fourcc);
	}
	case VCAM_IOC_G_FORMAT: {
		struct vcam_format fmt;

		mutex_lock(&vcam->lock);
		fmt.width = vcam->width;
		fmt.height = vcam->height;
		fmt.fourcc = vcam->fourcc;
		mutex_unlock(&vcam->lock);
		if (copy_to_user(argp, &fmt, sizeof(fmt)))
			return -EFAULT;
		return 0;
	}
	case VCAM_IOC_S_FPS: {
		struct vcam_fps fps;

		if (copy_from_user(&fps, argp, sizeof(fps)))
			return -EFAULT;
		return vcam_set_fps(vcam, fps.num, fps.den);
	}
	case VCAM_IOC_G_STATS: {
		struct vcam_stats stats;

		spin_lock_irq(&vcam->slock);
		stats.frames_submitted = vcam->frames_submitted;
		stats.frames_delivered = vcam->frames_delivered;
		stats.ticks_no_buffer = vcam->ticks_no_buffer;
		stats.sequence = vcam->sequence;
		spin_unlock_irq(&vcam->slock);
		if (copy_to_user(argp, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vcam_cdev_fops = {
	.owner		= THIS_MODULE,
	.write		= vcam_cdev_write,
	.unlocked_ioctl	= vcam_cdev_ioctl,
};

struct vcam_chardev *vcam_chardev_create(const struct vcam_config *cfg)
{
	struct vcam_chardev *cd;
	int ret;

	cd = kzalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return ERR_PTR(-ENOMEM);

	cd->vcam = vcam_create(NULL, cfg);
	if (IS_ERR(cd->vcam)) {
		ret = PTR_ERR(cd->vcam);
		goto err_free;
	}

	strscpy(cd->name, cfg->name, sizeof(cd->name));
	cd->misc.minor = MISC_DYNAMIC_MINOR;
	cd->misc.name = cd->name;
	cd->misc.fops = &vcam_cdev_fops;
	cd->misc.mode = 0666;

	ret = misc_register(&cd->misc);
	if (ret)
		goto err_vcam;

	pr_info("%s: %s: control node /dev/%s\n", VCAM_MODULE_NAME,
		cd->name, cd->name);
	return cd;

err_vcam:
	vcam_destroy(cd->vcam);
err_free:
	kfree(cd);
	return ERR_PTR(ret);
}

void vcam_chardev_destroy(struct vcam_chardev *cd)
{
	/*
	 * fops.owner pins the module while a control fd is open, so by the
	 * time module exit runs this, no writer can be mid-flight.
	 */
	misc_deregister(&cd->misc);
	vcam_destroy(cd->vcam);
	kfree(cd);
}
