// SPDX-License-Identifier: GPL-2.0
/*
 * vcam_replay - USB frontend (Phase 2).
 *
 * Binds to the QEMU "usb-vcam" device (see qemu-device/dev-vcam.c), asks it
 * for its fixed format via a vendor control request, and creates a V4L2
 * instance fed from the device's bulk IN endpoint.
 *
 * The device paces the stream (it emits one frame per 1/fps on its own
 * timer, like a real camera sensor), so the V4L2 core instance runs in
 * source-paced mode: every completed frame is delivered immediately.
 *
 * The bulk pipe is a byte stream of (struct vcam_frame_hdr, payload)
 * records; URB boundaries carry no meaning, so a small state machine
 * reassembles frames and resynchronizes on the header magic if the stream
 * ever gets out of step.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/usb.h>

#include "vcam.h"
#include "vcam_uapi.h"

#define VCAM_URB_BUF_SIZE	16384

enum vcam_rx_state {
	VCAM_RX_HDR,
	VCAM_RX_PAYLOAD,
	VCAM_RX_SKIP,
};

struct vcam_usb {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct vcam_dev *vcam;

	unsigned int bulk_in_pipe;
	struct urb *urb;
	u8 *urb_buf;
	bool running;		/* completion handler may resubmit */

	/* frame reassembly */
	enum vcam_rx_state state;
	struct vcam_frame_hdr hdr;
	u32 hdr_have;
	u8 *frame_buf;
	u32 frame_size;
	u32 frame_have;
	u32 skip_left;
};

static void vcam_usb_parse(struct vcam_usb *vu, const u8 *data, u32 len)
{
	while (len) {
		u32 take;

		switch (vu->state) {
		case VCAM_RX_HDR:
			take = min_t(u32, sizeof(vu->hdr) - vu->hdr_have, len);
			memcpy((u8 *)&vu->hdr + vu->hdr_have, data, take);
			vu->hdr_have += take;
			data += take;
			len -= take;
			if (vu->hdr_have < sizeof(vu->hdr))
				break;

			if (le32_to_cpu(vu->hdr.magic) != VCAM_FRAME_MAGIC) {
				/* resync: slide one byte and keep looking */
				memmove(&vu->hdr, (u8 *)&vu->hdr + 1,
					sizeof(vu->hdr) - 1);
				vu->hdr_have = sizeof(vu->hdr) - 1;
				break;
			}
			vu->hdr_have = 0;
			if (le32_to_cpu(vu->hdr.payload_len) !=
				    vu->frame_size ||
			    le16_to_cpu(vu->hdr.width) != vu->vcam->width ||
			    le16_to_cpu(vu->hdr.height) != vu->vcam->height ||
			    le32_to_cpu(vu->hdr.fourcc) != vu->vcam->fourcc) {
				dev_warn_ratelimited(&vu->intf->dev,
					"frame %u does not match negotiated format, skipping\n",
					le32_to_cpu(vu->hdr.seq));
				vu->state = VCAM_RX_SKIP;
				vu->skip_left =
					le32_to_cpu(vu->hdr.payload_len);
			} else {
				vu->state = VCAM_RX_PAYLOAD;
				vu->frame_have = 0;
			}
			break;

		case VCAM_RX_PAYLOAD:
			take = min_t(u32, vu->frame_size - vu->frame_have,
				     len);
			memcpy(vu->frame_buf + vu->frame_have, data, take);
			vu->frame_have += take;
			data += take;
			len -= take;
			if (vu->frame_have == vu->frame_size) {
				vcam_submit_frame(vu->vcam, vu->frame_buf,
						  vu->frame_size);
				vu->state = VCAM_RX_HDR;
			}
			break;

		case VCAM_RX_SKIP:
			take = min_t(u32, vu->skip_left, len);
			vu->skip_left -= take;
			data += take;
			len -= take;
			if (!vu->skip_left)
				vu->state = VCAM_RX_HDR;
			break;
		}
	}
}

static void vcam_usb_read_complete(struct urb *urb)
{
	struct vcam_usb *vu = urb->context;
	int ret;

	switch (urb->status) {
	case 0:
		vcam_usb_parse(vu, urb->transfer_buffer, urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;		/* killed or device gone: do not resubmit */
	default:
		dev_warn_ratelimited(&vu->intf->dev, "bulk read status %d\n",
				     urb->status);
		break;
	}

	if (READ_ONCE(vu->running)) {
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret && ret != -EPERM)
			dev_err(&vu->intf->dev, "urb resubmit failed: %d\n",
				ret);
	}
}

static int vcam_usb_set_stream(struct vcam_usb *vu, bool enable)
{
	return usb_control_msg_send(vu->udev, 0, VCAM_REQ_SET_STREAM,
				    USB_DIR_OUT | USB_TYPE_VENDOR |
					    USB_RECIP_DEVICE,
				    enable ? 1 : 0, 0, NULL, 0, 1000,
				    GFP_KERNEL);
}

/* Called from vcam core with vcam->lock held, process context. */
static void vcam_usb_on_stream(void *priv, bool enable)
{
	struct vcam_usb *vu = priv;
	int ret;

	if (enable) {
		vu->state = VCAM_RX_HDR;
		vu->hdr_have = 0;
		vu->frame_have = 0;
		WRITE_ONCE(vu->running, true);

		usb_fill_bulk_urb(vu->urb, vu->udev, vu->bulk_in_pipe,
				  vu->urb_buf, VCAM_URB_BUF_SIZE,
				  vcam_usb_read_complete, vu);
		ret = usb_submit_urb(vu->urb, GFP_KERNEL);
		if (ret) {
			dev_err(&vu->intf->dev, "urb submit failed: %d\n",
				ret);
			WRITE_ONCE(vu->running, false);
			return;
		}
		ret = vcam_usb_set_stream(vu, true);
		if (ret)
			dev_err(&vu->intf->dev, "stream start failed: %d\n",
				ret);
	} else {
		vcam_usb_set_stream(vu, false);
		WRITE_ONCE(vu->running, false);
		usb_kill_urb(vu->urb);
	}
}

static int vcam_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *bulk_in;
	struct vcam_usb *vu;
	struct vcam_usb_info info;
	struct vcam_config cfg = {};
	char busname[32];
	int ret;

	ret = usb_find_common_endpoints(intf->cur_altsetting, &bulk_in, NULL,
					NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "no bulk IN endpoint found\n");
		return ret;
	}

	vu = kzalloc(sizeof(*vu), GFP_KERNEL);
	if (!vu)
		return -ENOMEM;

	vu->udev = usb_get_dev(interface_to_usbdev(intf));
	vu->intf = intf;
	vu->bulk_in_pipe = usb_rcvbulkpipe(vu->udev,
					   usb_endpoint_num(bulk_in));

	ret = usb_control_msg_recv(vu->udev, 0, VCAM_REQ_GET_INFO,
				   USB_DIR_IN | USB_TYPE_VENDOR |
					   USB_RECIP_DEVICE,
				   0, 0, &info, sizeof(info), 1000,
				   GFP_KERNEL);
	if (ret) {
		dev_err(&intf->dev, "GET_INFO failed: %d\n", ret);
		goto err_put;
	}

	snprintf(busname, sizeof(busname), "usb-%s", dev_name(&intf->dev));
	cfg.width = le16_to_cpu(info.width);
	cfg.height = le16_to_cpu(info.height);
	cfg.fourcc = le32_to_cpu(info.fourcc);
	cfg.fps_num = le32_to_cpu(info.fps_num);
	cfg.fps_den = le32_to_cpu(info.fps_den);
	cfg.self_paced = false;	/* the emulated camera paces the stream */
	cfg.on_stream = vcam_usb_on_stream;
	cfg.priv = vu;
	cfg.name = "vcam-usb";
	cfg.bus_info = busname;

	vu->frame_size = vcam_sizeimage(cfg.width, cfg.height, cfg.fourcc);
	vu->frame_buf = vmalloc(vu->frame_size);
	vu->urb_buf = kmalloc(VCAM_URB_BUF_SIZE, GFP_KERNEL);
	vu->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!vu->frame_buf || !vu->urb_buf || !vu->urb) {
		ret = -ENOMEM;
		goto err_free;
	}

	vu->vcam = vcam_create(&intf->dev, &cfg);
	if (IS_ERR(vu->vcam)) {
		ret = PTR_ERR(vu->vcam);
		dev_err(&intf->dev, "vcam_create failed: %d\n", ret);
		goto err_free;
	}

	usb_set_intfdata(intf, vu);
	dev_info(&intf->dev, "vcam USB camera bound: %ux%u @ %u/%u fps\n",
		 cfg.width, cfg.height, cfg.fps_num, cfg.fps_den);
	return 0;

err_free:
	usb_free_urb(vu->urb);
	kfree(vu->urb_buf);
	vfree(vu->frame_buf);
err_put:
	usb_put_dev(vu->udev);
	kfree(vu);
	return ret;
}

static void vcam_usb_disconnect(struct usb_interface *intf)
{
	struct vcam_usb *vu = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);

	/* Waits for an in-flight on_stream and prevents further ones. */
	vcam_detach_producer(vu->vcam);
	WRITE_ONCE(vu->running, false);
	usb_kill_urb(vu->urb);

	vcam_destroy(vu->vcam);

	usb_free_urb(vu->urb);
	kfree(vu->urb_buf);
	vfree(vu->frame_buf);
	usb_put_dev(vu->udev);
	kfree(vu);
}

static const struct usb_device_id vcam_usb_ids[] = {
	{ USB_DEVICE(VCAM_USB_VID, VCAM_USB_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, vcam_usb_ids);

static struct usb_driver vcam_usb_driver = {
	.name = VCAM_MODULE_NAME,
	.id_table = vcam_usb_ids,
	.probe = vcam_usb_probe,
	.disconnect = vcam_usb_disconnect,
};

int vcam_usb_register(void)
{
	return usb_register(&vcam_usb_driver);
}

void vcam_usb_unregister(void)
{
	usb_deregister(&vcam_usb_driver);
}
