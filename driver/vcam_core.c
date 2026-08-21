// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_replay - V4L2 core: registers /dev/videoX, owns the videobuf2 queue
 * and delivers frames to it, either on its own hrtimer schedule (self-paced,
 * chardev frontend) or as the producer pushes them (USB frontend).
 *
 * Threading model
 * ---------------
 * One producer per instance (chardev writer or USB completion handler) and
 * one consumer (the delivery kthread). Frames move through a triple buffer:
 *
 *   producer: memcpy into buf_fill (no lock) -> swap fill/ready under slock
 *   consumer: swap ready/show under slock -> memcpy buf_show into the vb2
 *             buffer (no lock)
 *
 * so the large copies never happen under the spinlock. The hrtimer only
 * marks a delivery tick and wakes the kthread; it is kept drift-free with
 * hrtimer_forward_now(), which advances the expiry on the timer's own
 * timeline rather than relative to when the callback happened to run.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-common.h>
#include <media/videobuf2-vmalloc.h>

#include "vcam.h"

struct vcam_buf {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

static inline struct vcam_buf *to_vcam_buf(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct vcam_buf, vb);
}

int vcam_fourcc_valid(u32 fourcc)
{
	return fourcc == V4L2_PIX_FMT_GREY || fourcc == V4L2_PIX_FMT_YUYV;
}

static u32 vcam_bytesperline(const struct vcam_dev *vcam)
{
	return vcam->fourcc == V4L2_PIX_FMT_YUYV ? vcam->width * 2
						 : vcam->width;
}

/*
 * Diagonal gradient so that streaming shows something sensible before the
 * first real frame arrives (and so timing can be validated with no feeder).
 */
static void vcam_fill_pattern(u8 *buf, u32 width, u32 height, u32 fourcc)
{
	u32 x, y;

	if (fourcc == V4L2_PIX_FMT_YUYV) {
		for (y = 0; y < height; y++) {
			u8 *line = buf + y * width * 2;

			for (x = 0; x < width; x++) {
				line[x * 2] = (x + y) & 0xff;	/* Y */
				line[x * 2 + 1] = 0x80;		/* U/V */
			}
		}
	} else {
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
				buf[y * width + x] = (x + y) & 0xff;
	}
}

/* ------------------------------------------------------------------ */
/* frame delivery                                                     */

static enum hrtimer_restart vcam_timer_fn(struct hrtimer *timer)
{
	struct vcam_dev *vcam = container_of(timer, struct vcam_dev, timer);
	ktime_t period;
	unsigned long flags;

	spin_lock_irqsave(&vcam->slock, flags);
	vcam->deliver_pending = true;
	period = vcam->period;
	spin_unlock_irqrestore(&vcam->slock, flags);

	wake_up(&vcam->wq);
	hrtimer_forward_now(timer, period);
	return HRTIMER_RESTART;
}

static void vcam_deliver_one(struct vcam_dev *vcam)
{
	struct vcam_buf *buf;
	void *vaddr;
	u32 seq;

	mutex_lock(&vcam->deliver_lock);

	spin_lock_irq(&vcam->slock);
	if (!vcam->streaming) {
		spin_unlock_irq(&vcam->slock);
		goto out;
	}
	if (vcam->ready_valid) {
		swap(vcam->buf_ready, vcam->buf_show);
		vcam->ready_valid = false;
	}
	buf = list_first_entry_or_null(&vcam->buf_list, struct vcam_buf, list);
	if (buf)
		list_del(&buf->list);
	else
		vcam->ticks_no_buffer++;
	seq = vcam->sequence++;
	spin_unlock_irq(&vcam->slock);

	if (!buf)
		goto out;

	vaddr = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	memcpy(vaddr, vcam->buf_show, vcam->sizeimage);
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, vcam->sizeimage);
	buf->vb.sequence = seq;
	buf->vb.field = V4L2_FIELD_NONE;
	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);

	spin_lock_irq(&vcam->slock);
	vcam->frames_delivered++;
	spin_unlock_irq(&vcam->slock);
out:
	mutex_unlock(&vcam->deliver_lock);
}

static int vcam_thread_fn(void *data)
{
	struct vcam_dev *vcam = data;

	for (;;) {
		wait_event_interruptible(vcam->wq,
					 READ_ONCE(vcam->deliver_pending) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		spin_lock_irq(&vcam->slock);
		vcam->deliver_pending = false;
		spin_unlock_irq(&vcam->slock);

		vcam_deliver_one(vcam);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* producer API                                                       */

static void vcam_commit_fill_buf(struct vcam_dev *vcam)
{
	bool wake = false;
	unsigned long flags;

	spin_lock_irqsave(&vcam->slock, flags);
	swap(vcam->buf_fill, vcam->buf_ready);
	vcam->ready_valid = true;
	vcam->frames_submitted++;
	if (!vcam->self_paced && vcam->streaming) {
		vcam->deliver_pending = true;
		wake = true;
	}
	spin_unlock_irqrestore(&vcam->slock, flags);

	if (wake)
		wake_up(&vcam->wq);
}

int vcam_submit_frame(struct vcam_dev *vcam, const void *data, size_t len)
{
	if (len != vcam->sizeimage)
		return -EINVAL;

	memcpy(vcam->buf_fill, data, len);
	vcam_commit_fill_buf(vcam);
	return 0;
}

int vcam_submit_frame_user(struct vcam_dev *vcam, const char __user *data,
			   size_t len)
{
	int ret = 0;

	/* Excludes concurrent writers and format changes. */
	mutex_lock(&vcam->lock);
	if (vcam->dying) {
		ret = -ENODEV;
	} else if (len != vcam->sizeimage) {
		ret = -EINVAL;
	} else if (copy_from_user(vcam->buf_fill, data, len)) {
		ret = -EFAULT;
	} else {
		vcam_commit_fill_buf(vcam);
	}
	mutex_unlock(&vcam->lock);
	return ret;
}

/* ------------------------------------------------------------------ */
/* format / rate control                                              */

static int vcam_alloc_frame_bufs(struct vcam_dev *vcam)
{
	u32 size = vcam->sizeimage;
	u8 *a, *b, *c;

	a = vmalloc(size);
	b = vmalloc(size);
	c = vmalloc(size);
	if (!a || !b || !c) {
		vfree(a);
		vfree(b);
		vfree(c);
		return -ENOMEM;
	}
	vcam_fill_pattern(a, vcam->width, vcam->height, vcam->fourcc);
	memcpy(b, a, size);
	memcpy(c, a, size);

	vfree(vcam->buf_fill);
	vfree(vcam->buf_ready);
	vfree(vcam->buf_show);
	vcam->buf_fill = a;
	vcam->buf_ready = b;
	vcam->buf_show = c;
	vcam->ready_valid = false;
	return 0;
}

static int vcam_check_format(u32 width, u32 height, u32 fourcc)
{
	if (!vcam_fourcc_valid(fourcc))
		return -EINVAL;
	if (width < VCAM_MIN_DIM || width > VCAM_MAX_DIM ||
	    height < VCAM_MIN_DIM || height > VCAM_MAX_DIM)
		return -EINVAL;
	if (fourcc == V4L2_PIX_FMT_YUYV && (width & 1))
		return -EINVAL;
	return 0;
}

int vcam_set_format(struct vcam_dev *vcam, u32 width, u32 height, u32 fourcc)
{
	int ret;

	ret = vcam_check_format(width, height, fourcc);
	if (ret)
		return ret;

	mutex_lock(&vcam->lock);
	if (vcam->dying) {
		ret = -ENODEV;
		goto out;
	}
	if (vb2_is_busy(&vcam->queue)) {
		ret = -EBUSY;
		goto out;
	}
	if (width == vcam->width && height == vcam->height &&
	    fourcc == vcam->fourcc)
		goto out;

	vcam->width = width;
	vcam->height = height;
	vcam->fourcc = fourcc;
	vcam->sizeimage = vcam_sizeimage(width, height, fourcc);
	ret = vcam_alloc_frame_bufs(vcam);
out:
	mutex_unlock(&vcam->lock);
	return ret;
}

int vcam_set_fps(struct vcam_dev *vcam, u32 num, u32 den)
{
	u64 interval_ns;
	unsigned long flags;

	if (!num || !den)
		return -EINVAL;

	interval_ns = div_u64((u64)NSEC_PER_SEC * den, num);
	/* 100000 fps cap (interrupt storm guard) .. 1 frame/minute floor */
	if (interval_ns < 10 * NSEC_PER_USEC || interval_ns > 60 * NSEC_PER_SEC)
		return -EINVAL;

	spin_lock_irqsave(&vcam->slock, flags);
	vcam->fps_num = num;
	vcam->fps_den = den;
	/* picked up by hrtimer_forward_now() on the next tick */
	vcam->period = ns_to_ktime(interval_ns);
	spin_unlock_irqrestore(&vcam->slock, flags);
	return 0;
}

/* ------------------------------------------------------------------ */
/* videobuf2                                                          */

static int vcam_queue_setup(struct vb2_queue *q, unsigned int *num_buffers,
			    unsigned int *num_planes, unsigned int sizes[],
			    struct device *alloc_devs[])
{
	struct vcam_dev *vcam = vb2_get_drv_priv(q);

	if (*num_planes)
		return sizes[0] < vcam->sizeimage ? -EINVAL : 0;

	*num_planes = 1;
	sizes[0] = vcam->sizeimage;
	return 0;
}

static int vcam_buf_prepare(struct vb2_buffer *vb)
{
	struct vcam_dev *vcam = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < vcam->sizeimage)
		return -EINVAL;
	return 0;
}

static void vcam_buf_queue(struct vb2_buffer *vb)
{
	struct vcam_dev *vcam = vb2_get_drv_priv(vb->vb2_queue);
	struct vcam_buf *buf = to_vcam_buf(vb);
	unsigned long flags;

	spin_lock_irqsave(&vcam->slock, flags);
	list_add_tail(&buf->list, &vcam->buf_list);
	spin_unlock_irqrestore(&vcam->slock, flags);
}

static void vcam_return_all_buffers(struct vcam_dev *vcam,
				    enum vb2_buffer_state state)
{
	struct vcam_buf *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&vcam->slock, flags);
	list_for_each_entry_safe(buf, tmp, &vcam->buf_list, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&vcam->slock, flags);
}

static int vcam_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct vcam_dev *vcam = vb2_get_drv_priv(q);
	ktime_t period;

	if (vcam->dying) {
		vcam_return_all_buffers(vcam, VB2_BUF_STATE_QUEUED);
		return -ENODEV;
	}

	spin_lock_irq(&vcam->slock);
	vcam->sequence = 0;
	vcam->streaming = true;
	vcam->deliver_pending = false;
	period = vcam->period;
	spin_unlock_irq(&vcam->slock);

	if (vcam->on_stream)
		vcam->on_stream(vcam->stream_priv, true);

	if (vcam->self_paced)
		hrtimer_start(&vcam->timer, period, HRTIMER_MODE_REL);
	return 0;
}

static void vcam_stop_streaming(struct vb2_queue *q)
{
	struct vcam_dev *vcam = vb2_get_drv_priv(q);

	spin_lock_irq(&vcam->slock);
	vcam->streaming = false;
	vcam->deliver_pending = false;
	spin_unlock_irq(&vcam->slock);

	if (vcam->self_paced)
		hrtimer_cancel(&vcam->timer);

	/* Wait out a delivery that already popped a buffer. */
	mutex_lock(&vcam->deliver_lock);
	mutex_unlock(&vcam->deliver_lock);

	vcam_return_all_buffers(vcam, VB2_BUF_STATE_ERROR);

	if (vcam->on_stream)
		vcam->on_stream(vcam->stream_priv, false);
}

static const struct vb2_ops vcam_vb2_ops = {
	.queue_setup		= vcam_queue_setup,
	.buf_prepare		= vcam_buf_prepare,
	.buf_queue		= vcam_buf_queue,
	.start_streaming	= vcam_start_streaming,
	.stop_streaming		= vcam_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

/* ------------------------------------------------------------------ */
/* V4L2 ioctls                                                        */

static int vcam_querycap(struct file *file, void *priv,
			 struct v4l2_capability *cap)
{
	struct vcam_dev *vcam = video_drvdata(file);

	strscpy(cap->driver, VCAM_MODULE_NAME, sizeof(cap->driver));
	strscpy(cap->card, vcam->name, sizeof(cap->card));
	strscpy(cap->bus_info, vcam->bus_info, sizeof(cap->bus_info));
	return 0;
}

static void vcam_fill_pix_format(struct vcam_dev *vcam,
				 struct v4l2_pix_format *pix)
{
	memset(pix, 0, sizeof(*pix));
	pix->width = vcam->width;
	pix->height = vcam->height;
	pix->pixelformat = vcam->fourcc;
	pix->field = V4L2_FIELD_NONE;
	pix->bytesperline = vcam_bytesperline(vcam);
	pix->sizeimage = vcam->sizeimage;
	pix->colorspace = V4L2_COLORSPACE_SRGB;
}

/*
 * Like a fixed-format camera, the producer (feeder or emulated USB device)
 * defines the format; V4L2 clients only get to see it. S_FMT/TRY_FMT
 * therefore coerce everything to the current format.
 */
static int vcam_enum_fmt_vid_cap(struct file *file, void *priv,
				 struct v4l2_fmtdesc *f)
{
	struct vcam_dev *vcam = video_drvdata(file);

	if (f->index > 0)
		return -EINVAL;
	f->pixelformat = vcam->fourcc;
	return 0;
}

static int vcam_g_fmt_vid_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	struct vcam_dev *vcam = video_drvdata(file);

	vcam_fill_pix_format(vcam, &f->fmt.pix);
	return 0;
}

static int vcam_try_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vcam_g_fmt_vid_cap(file, priv, f);
}

static int vcam_s_fmt_vid_cap(struct file *file, void *priv,
			      struct v4l2_format *f)
{
	return vcam_g_fmt_vid_cap(file, priv, f);
}

static int vcam_enum_framesizes(struct file *file, void *priv,
				struct v4l2_frmsizeenum *fsize)
{
	struct vcam_dev *vcam = video_drvdata(file);

	if (fsize->index > 0 || fsize->pixel_format != vcam->fourcc)
		return -EINVAL;
	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete.width = vcam->width;
	fsize->discrete.height = vcam->height;
	return 0;
}

static int vcam_enum_frameintervals(struct file *file, void *priv,
				    struct v4l2_frmivalenum *fival)
{
	struct vcam_dev *vcam = video_drvdata(file);

	if (fival->index > 0 || fival->pixel_format != vcam->fourcc ||
	    fival->width != vcam->width || fival->height != vcam->height)
		return -EINVAL;
	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = vcam->fps_den;
	fival->discrete.denominator = vcam->fps_num;
	return 0;
}

static int vcam_g_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *parm)
{
	struct vcam_dev *vcam = video_drvdata(file);

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	memset(&parm->parm.capture, 0, sizeof(parm->parm.capture));
	parm->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	parm->parm.capture.timeperframe.numerator = vcam->fps_den;
	parm->parm.capture.timeperframe.denominator = vcam->fps_num;
	parm->parm.capture.readbuffers = 2;
	return 0;
}

static int vcam_s_parm(struct file *file, void *priv,
		       struct v4l2_streamparm *parm)
{
	struct vcam_dev *vcam = video_drvdata(file);
	struct v4l2_fract *tpf = &parm->parm.capture.timeperframe;

	if (parm->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	/* timeperframe is seconds/frame, our fps fraction is frames/second */
	if (vcam->self_paced && tpf->numerator && tpf->denominator)
		vcam_set_fps(vcam, tpf->denominator, tpf->numerator);
	return vcam_g_parm(file, priv, parm);
}

static const struct v4l2_ioctl_ops vcam_ioctl_ops = {
	.vidioc_querycap		= vcam_querycap,
	.vidioc_enum_fmt_vid_cap	= vcam_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap		= vcam_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap		= vcam_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap		= vcam_s_fmt_vid_cap,
	.vidioc_enum_framesizes		= vcam_enum_framesizes,
	.vidioc_enum_frameintervals	= vcam_enum_frameintervals,
	.vidioc_g_parm			= vcam_g_parm,
	.vidioc_s_parm			= vcam_s_parm,

	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_prepare_buf		= vb2_ioctl_prepare_buf,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations vcam_fops = {
	.owner		= THIS_MODULE,
	.open		= v4l2_fh_open,
	.release	= vb2_fop_release,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
	.unlocked_ioctl	= video_ioctl2,
};

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */

static void vcam_video_release(struct video_device *vdev)
{
	struct vcam_dev *vcam = container_of(vdev, struct vcam_dev, vdev);

	vfree(vcam->buf_fill);
	vfree(vcam->buf_ready);
	vfree(vcam->buf_show);
	kfree(vcam);
}

struct vcam_dev *vcam_create(struct device *parent,
			     const struct vcam_config *cfg)
{
	struct vcam_dev *vcam;
	struct video_device *vdev;
	struct vb2_queue *q;
	int ret;

	ret = vcam_check_format(cfg->width, cfg->height, cfg->fourcc);
	if (ret)
		return ERR_PTR(ret);

	vcam = kzalloc(sizeof(*vcam), GFP_KERNEL);
	if (!vcam)
		return ERR_PTR(-ENOMEM);

	mutex_init(&vcam->lock);
	mutex_init(&vcam->deliver_lock);
	spin_lock_init(&vcam->slock);
	INIT_LIST_HEAD(&vcam->buf_list);
	init_waitqueue_head(&vcam->wq);

	strscpy(vcam->name, cfg->name ?: "vcam", sizeof(vcam->name));
	strscpy(vcam->bus_info, cfg->bus_info ?: "platform:vcam_replay",
		sizeof(vcam->bus_info));

	vcam->width = cfg->width;
	vcam->height = cfg->height;
	vcam->fourcc = cfg->fourcc;
	vcam->sizeimage = vcam_sizeimage(cfg->width, cfg->height, cfg->fourcc);
	vcam->self_paced = cfg->self_paced;
	vcam->on_stream = cfg->on_stream;
	vcam->stream_priv = cfg->priv;

	ret = vcam_set_fps(vcam, cfg->fps_num, cfg->fps_den);
	if (ret)
		goto err_free;

	ret = vcam_alloc_frame_bufs(vcam);
	if (ret)
		goto err_free;

	hrtimer_init(&vcam->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	vcam->timer.function = vcam_timer_fn;

	vcam->thread = kthread_run(vcam_thread_fn, vcam, "%s-deliver",
				   vcam->name);
	if (IS_ERR(vcam->thread)) {
		ret = PTR_ERR(vcam->thread);
		goto err_bufs;
	}

	ret = v4l2_device_register(parent, &vcam->v4l2_dev);
	if (ret)
		goto err_thread;

	q = &vcam->queue;
	q->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_READ;
	q->drv_priv = vcam;
	q->buf_struct_size = sizeof(struct vcam_buf);
	q->ops = &vcam_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &vcam->lock;
	ret = vb2_queue_init(q);
	if (ret)
		goto err_v4l2;

	vdev = &vcam->vdev;
	strscpy(vdev->name, vcam->name, sizeof(vdev->name));
	vdev->v4l2_dev = &vcam->v4l2_dev;
	vdev->fops = &vcam_fops;
	vdev->ioctl_ops = &vcam_ioctl_ops;
	vdev->queue = q;
	vdev->lock = &vcam->lock;
	vdev->release = vcam_video_release;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, vcam);

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret)
		goto err_v4l2;

	v4l2_info(&vcam->v4l2_dev,
		  "%s: registered %s, %ux%u %c%c%c%c @ %u/%u fps (%s)\n",
		  vcam->name, video_device_node_name(vdev),
		  vcam->width, vcam->height,
		  vcam->fourcc & 0xff, (vcam->fourcc >> 8) & 0xff,
		  (vcam->fourcc >> 16) & 0xff, (vcam->fourcc >> 24) & 0xff,
		  vcam->fps_num, vcam->fps_den,
		  vcam->self_paced ? "self-paced" : "source-paced");
	return vcam;

err_v4l2:
	v4l2_device_unregister(&vcam->v4l2_dev);
err_thread:
	kthread_stop(vcam->thread);
err_bufs:
	vfree(vcam->buf_fill);
	vfree(vcam->buf_ready);
	vfree(vcam->buf_show);
err_free:
	kfree(vcam);
	return ERR_PTR(ret);
}

void vcam_detach_producer(struct vcam_dev *vcam)
{
	mutex_lock(&vcam->lock);
	vcam->on_stream = NULL;
	vcam->stream_priv = NULL;
	mutex_unlock(&vcam->lock);
}

void vcam_destroy(struct vcam_dev *vcam)
{
	mutex_lock(&vcam->lock);
	vcam->dying = true;
	mutex_unlock(&vcam->lock);

	/* Stops streaming (if active) and blocks new opens/STREAMONs. */
	vb2_video_unregister_device(&vcam->vdev);

	hrtimer_cancel(&vcam->timer);
	kthread_stop(vcam->thread);
	v4l2_device_unregister(&vcam->v4l2_dev);
	/*
	 * Frame buffers and the vcam struct itself are freed by
	 * vcam_video_release() once the last open fd is closed
	 * (immediately, if none is open).
	 */
}
