/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vcam_replay - userspace ABI and USB wire protocol
 *
 * Shared between:
 *   - the kernel module (driver/)
 *   - the userspace feeder (tools/vcam_feed.c)
 *   - the QEMU device model (qemu-device/dev-vcam.c keeps its own copy of
 *     the wire-protocol constants; keep them in sync)
 */
#ifndef VCAM_UAPI_H
#define VCAM_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * ---------------------------------------------------------------------
 * Control character device (/dev/vcam0, one per module-created instance)
 *
 * write(): exactly one full frame per call; the byte count must equal
 * the sizeimage of the current format (width*height for GREY,
 * width*height*2 for YUYV). Short/oversized writes fail with -EINVAL.
 * ---------------------------------------------------------------------
 */

struct vcam_format {
	__u32 width;
	__u32 height;
	__u32 fourcc;		/* V4L2_PIX_FMT_GREY or V4L2_PIX_FMT_YUYV */
};

struct vcam_fps {
	__u32 num;		/* frames per second = num / den */
	__u32 den;
};

struct vcam_stats {
	__u64 frames_submitted;	/* frames pushed in by the producer */
	__u64 frames_delivered;	/* frames handed to V4L2 buffers */
	__u64 ticks_no_buffer;	/* delivery ticks with no buffer queued */
	__u64 sequence;		/* current V4L2 sequence counter */
};

#define VCAM_IOC_MAGIC	'r'
#define VCAM_IOC_S_FORMAT	_IOW(VCAM_IOC_MAGIC, 0x01, struct vcam_format)
#define VCAM_IOC_G_FORMAT	_IOR(VCAM_IOC_MAGIC, 0x02, struct vcam_format)
#define VCAM_IOC_S_FPS		_IOW(VCAM_IOC_MAGIC, 0x03, struct vcam_fps)
#define VCAM_IOC_G_STATS	_IOR(VCAM_IOC_MAGIC, 0x04, struct vcam_stats)

/*
 * ---------------------------------------------------------------------
 * USB wire protocol (must match qemu-device/dev-vcam.c)
 * ---------------------------------------------------------------------
 */

#define VCAM_USB_VID	0x1209	/* pid.codes */
#define VCAM_USB_PID	0x000a	/* pid.codes test PID range */

/* Vendor control requests, bmRequestType = vendor | device */
#define VCAM_REQ_GET_INFO	0x01	/* IN:  struct vcam_usb_info */
#define VCAM_REQ_SET_STREAM	0x02	/* OUT: wValue = 1 start / 0 stop */
#define VCAM_REQ_SET_FPS	0x03	/* OUT: wValue = integer fps */

struct vcam_usb_info {		/* little-endian on the wire, 16 bytes */
	__le16 width;
	__le16 height;
	__le32 fourcc;
	__le32 fps_num;
	__le32 fps_den;
} __attribute__((packed));

/* Bulk IN pipe carries a stream of (header, payload) pairs. */
#define VCAM_FRAME_MAGIC	0x4d414356	/* "VCAM" little-endian */

struct vcam_frame_hdr {		/* little-endian on the wire, 32 bytes */
	__le32 magic;
	__le32 seq;
	__le16 width;
	__le16 height;
	__le32 fourcc;
	__le32 payload_len;
	__le32 flags;
	__le32 reserved[2];
} __attribute__((packed));

#endif /* VCAM_UAPI_H */
