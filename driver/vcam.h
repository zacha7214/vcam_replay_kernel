/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vcam_replay - internal API between the V4L2 core and its frame producers
 * (character-device frontend and USB frontend).
 */
#ifndef VCAM_H
#define VCAM_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-v4l2.h>

#define VCAM_MODULE_NAME	"vcam_replay"

#define VCAM_MIN_DIM		16
#define VCAM_MAX_DIM		8192

struct vcam_config {
	u32 width;
	u32 height;
	u32 fourcc;		/* V4L2_PIX_FMT_GREY or V4L2_PIX_FMT_YUYV */
	u32 fps_num;		/* fps = fps_num / fps_den */
	u32 fps_den;
	/*
	 * self_paced: the core delivers the most recent frame to V4L2 on an
	 * hrtimer at the configured fps (chardev frontend: the producer just
	 * replaces "the latest frame").
	 *
	 * !self_paced: the producer defines the timing; every submitted frame
	 * is delivered immediately (USB frontend: the emulated camera paces).
	 */
	bool self_paced;
	/*
	 * Optional producer hook, invoked with the core mutex held from
	 * process context when V4L2 streaming starts/stops. May sleep.
	 * The USB frontend uses this to start/stop the device-side stream.
	 */
	int (*on_stream)(void *priv, bool enable);
	void *priv;
	const char *name;	/* short instance name, e.g. "vcam0" */
	const char *bus_info;	/* for VIDIOC_QUERYCAP */
};

struct vcam_dev {
	struct v4l2_device v4l2_dev;
	struct video_device vdev;
	struct vb2_queue queue;
	/* serializes ioctls, queue ops, format changes and chardev writes */
	struct mutex lock;
	/* serializes one in-flight delivery against stop_streaming */
	struct mutex deliver_lock;
	/* protects buf_list, frame swap state, counters, period */
	spinlock_t slock;
	struct list_head buf_list;

	char name[32];
	char bus_info[32];

	/* current (producer-defined) format */
	u32 width, height, fourcc;
	u32 sizeimage;
	u32 fps_num, fps_den;

	bool self_paced;
	bool streaming;		/* under slock */
	bool dying;		/* under lock; blocks new STREAMON/submits */

	int (*on_stream)(void *priv, bool enable);
	void *stream_priv;

	/* pacing */
	struct hrtimer timer;
	ktime_t period;		/* under slock */
	struct task_struct *thread;
	wait_queue_head_t wq;
	bool deliver_pending;	/* under slock */

	/*
	 * Triple buffering: the single producer fills buf_fill (unlocked),
	 * then swaps it with buf_ready under slock. The single consumer
	 * (delivery thread) swaps buf_ready into buf_show under slock and
	 * copies from buf_show unlocked.
	 */
	u8 *buf_fill, *buf_ready, *buf_show;
	bool ready_valid;	/* under slock */

	/* stats, under slock */
	u64 frames_submitted;
	u64 frames_delivered;
	u64 ticks_no_buffer;
	u32 sequence;
};

struct vcam_dev *vcam_create(struct device *parent,
			     const struct vcam_config *cfg);
void vcam_destroy(struct vcam_dev *vcam);
/* Disarm on_stream/priv before the producer goes away (USB disconnect). */
void vcam_detach_producer(struct vcam_dev *vcam);

int vcam_set_format(struct vcam_dev *vcam, u32 width, u32 height, u32 fourcc);
int vcam_set_fps(struct vcam_dev *vcam, u32 num, u32 den);

/* Producers: kernel-space frame (USB completion, softirq-safe). */
int vcam_submit_frame(struct vcam_dev *vcam, const void *data, size_t len);
/* Producers: userspace frame (chardev write, process context). */
int vcam_submit_frame_user(struct vcam_dev *vcam, const char __user *data,
			   size_t len);

static inline u32 vcam_sizeimage(u32 width, u32 height, u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_YUYV ? width * height * 2
					   : width * height;
}

int vcam_fourcc_valid(u32 fourcc);
int vcam_check_format(u32 width, u32 height, u32 fourcc);

/* chardev frontend (vcam_chardev.c) */
struct vcam_chardev;
struct vcam_chardev *vcam_chardev_create(const struct vcam_config *cfg);
void vcam_chardev_destroy(struct vcam_chardev *cd);

/* USB frontend (vcam_usb.c) */
int vcam_usb_register(void);
void vcam_usb_unregister(void);

#endif /* VCAM_H */
