/*
 * QEMU USB replay-camera device ("usb-vcam").
 *
 * A vendor-specific USB device that emits video frames on a bulk IN
 * endpoint at a fixed frame rate, driven by a QEMU virtual-clock timer.
 * The matching guest driver is vcam_replay (driver/vcam_usb.c in the
 * vcam_replay_kernel project), which exposes the stream as /dev/videoX.
 *
 * Current functionality (intentionally minimal, meant to be extended):
 *   - enumeration as VID:PID 0x1209:0x000a, class 0xff (vendor)
 *   - vendor control requests:
 *       GET_INFO   (0x01, IN)  -> 16-byte format/rate description
 *       SET_STREAM (0x02, OUT) -> wValue 1/0 starts/stops the frame timer
 *       SET_FPS    (0x03, OUT) -> wValue = new integer frame rate
 *   - while streaming, one (32-byte header + payload) record per frame
 *     period on the bulk pipe, payload is a generated moving test pattern
 *
 * The optional frames= property replays a host file of packed raw frames.
 *
 * Wire protocol constants below must stay in sync with the guest driver's
 * driver/vcam_uapi.h.
 *
 * Usage:
 *   -device qemu-xhci -device usb-vcam,width=640,height=480,fps=30
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/usb/usb.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "qom/object.h"

/* ---- wire protocol (keep in sync with driver/vcam_uapi.h) ---- */

#define VCAM_USB_VID		0x1209
#define VCAM_USB_PID		0x000a

#define VCAM_REQ_GET_INFO	0x01
#define VCAM_REQ_SET_STREAM	0x02
#define VCAM_REQ_SET_FPS	0x03

#define VCAM_INFO_SIZE		16	/* GET_INFO response */
#define VCAM_FRAME_HDR_SIZE	32	/* per-frame bulk header */
#define VCAM_FRAME_MAGIC	0x4d414356	/* "VCAM" LE */

#define VCAM_FOURCC(a, b, c, d) \
	((uint32_t)(a) | ((uint32_t)(b) << 8) | \
	 ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define VCAM_FMT_GREY	VCAM_FOURCC('G', 'R', 'E', 'Y')
#define VCAM_FMT_YUYV	VCAM_FOURCC('Y', 'U', 'Y', 'V')

#define VCAM_MIN_DIM	16
#define VCAM_MAX_DIM	8192
#define VCAM_MAX_FPS	100000		/* Basler replay target is 2000 */

/* ---- device state ---- */

struct USBVCamState {
    USBDevice dev;
    USBEndpoint *bulk_in;
    QEMUTimer *frame_timer;
    int64_t next_frame_ns;
    int64_t frame_interval_ns;

    /* properties */
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    char *format;
    char *frames;
    GMappedFile *frame_file;
    size_t frame_count;
    size_t frame_index;

    uint32_t fourcc;
    uint32_t payload_size;

    bool streaming;
    uint32_t seq;
    uint32_t frames_dropped;

    /* current in-flight transfer: header + payload */
    uint8_t *xfer_buf;
    uint32_t xfer_len;
    uint32_t xfer_pos;
    bool frame_pending;
};

#define TYPE_USB_VCAM "usb-vcam"
OBJECT_DECLARE_SIMPLE_TYPE(USBVCamState, USB_VCAM)

/* ---- descriptors ---- */

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "QEMU",
    [STR_PRODUCT]      = "QEMU Replay Camera",
    [STR_SERIALNUMBER] = "1",
};

static const USBDescIface desc_iface_vcam_full = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = 1,
    .bInterfaceClass    = 0xff,     /* vendor specific */
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress = USB_DIR_IN | 0x01,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 64,
        },
    },
};

static const USBDescIface desc_iface_vcam_high = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = 1,
    .bInterfaceClass    = 0xff,
    .bInterfaceSubClass = 0x00,
    .bInterfaceProtocol = 0x00,
    .eps = (USBDescEndpoint[]) {
        {
            .bEndpointAddress = USB_DIR_IN | 0x01,
            .bmAttributes     = USB_ENDPOINT_XFER_BULK,
            .wMaxPacketSize   = 512,
        },
    },
};

static const USBDescDevice desc_device_vcam_full = {
    .bcdUSB             = 0x0200,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE,
            .bMaxPower           = 50,
            .nif = 1,
            .ifs = &desc_iface_vcam_full,
        },
    },
};

static const USBDescDevice desc_device_vcam_high = {
    .bcdUSB             = 0x0200,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE,
            .bMaxPower           = 50,
            .nif = 1,
            .ifs = &desc_iface_vcam_high,
        },
    },
};

static const USBDesc desc_vcam = {
    .id = {
        .idVendor      = VCAM_USB_VID,
        .idProduct     = VCAM_USB_PID,
        .bcdDevice     = 0x0100,
        .iManufacturer = STR_MANUFACTURER,
        .iProduct      = STR_PRODUCT,
        .iSerialNumber = STR_SERIALNUMBER,
    },
    .full = &desc_device_vcam_full,
    .high = &desc_device_vcam_high,
    .str  = desc_strings,
};

/* ---- frame generation ---- */

/*
 * Scrolling diagonal gradient plus a bright vertical bar that advances one
 * step per frame: makes both dropped frames and wrong pacing obvious by eye.
 */
static void vcam_generate_pattern(USBVCamState *s, uint8_t *buf)
{
    uint32_t bar = (s->seq * 4) % s->width;
    uint32_t x, y;

    if (s->fourcc == VCAM_FMT_YUYV) {
        for (y = 0; y < s->height; y++) {
            uint8_t *line = buf + y * s->width * 2;

            for (x = 0; x < s->width; x++) {
                uint8_t v = (x + y + s->seq * 3) & 0xff;

                line[x * 2]     = (x == bar) ? 0xff : v;    /* Y */
                line[x * 2 + 1] = 0x80;                     /* U/V */
            }
        }
    } else {
        for (y = 0; y < s->height; y++) {
            uint8_t *line = buf + y * s->width;

            for (x = 0; x < s->width; x++) {
                line[x] = (x == bar) ? 0xff : ((x + y + s->seq * 3) & 0xff);
            }
        }
    }
}

static void vcam_frame_timer(void *opaque)
{
    USBVCamState *s = opaque;
    uint8_t *hdr = s->xfer_buf;
    int64_t now;

    if (!s->streaming) {
        return;
    }

    /*
     * If the guest is still draining the previous frame, drop this one
     * rather than corrupt the byte stream mid-transfer. An untouched
     * pending frame is simply overwritten (latest frame wins), which is
     * what a real free-running sensor does.
     */
    if (s->frame_pending && s->xfer_pos > 0) {
        s->frames_dropped++;
    } else {
        stl_le_p(hdr + 0, VCAM_FRAME_MAGIC);
        stl_le_p(hdr + 4, s->seq);
        stw_le_p(hdr + 8, s->width);
        stw_le_p(hdr + 10, s->height);
        stl_le_p(hdr + 12, s->fourcc);
        stl_le_p(hdr + 16, s->payload_size);
        stl_le_p(hdr + 20, 0);                  /* flags */
        stl_le_p(hdr + 24, 0);
        stl_le_p(hdr + 28, 0);
        if (s->frame_file) {
            const char *frames = g_mapped_file_get_contents(s->frame_file);

            memcpy(s->xfer_buf + VCAM_FRAME_HDR_SIZE,
                   frames + s->frame_index * s->payload_size, s->payload_size);
            s->frame_index = (s->frame_index + 1) % s->frame_count;
        } else {
            vcam_generate_pattern(s, s->xfer_buf + VCAM_FRAME_HDR_SIZE);
        }

        s->xfer_len = VCAM_FRAME_HDR_SIZE + s->payload_size;
        s->xfer_pos = 0;
        s->frame_pending = true;
        usb_wakeup(s->bulk_in, 0);
    }
    s->seq++;

    /* drift-free schedule: advance on our own timeline, resync if behind */
    s->next_frame_ns += s->frame_interval_ns;
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (s->next_frame_ns <= now) {
        s->next_frame_ns = now + s->frame_interval_ns;
    }
    timer_mod(s->frame_timer, s->next_frame_ns);
}

static void vcam_set_interval(USBVCamState *s)
{
    s->frame_interval_ns = NANOSECONDS_PER_SECOND / s->fps;
}

static void vcam_stream_start(USBVCamState *s)
{
    if (s->streaming) {
        return;
    }
    s->streaming = true;
    s->frame_index = 0;
    s->frame_pending = false;
    s->xfer_pos = 0;
    s->next_frame_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                       s->frame_interval_ns;
    timer_mod(s->frame_timer, s->next_frame_ns);
}

static void vcam_stream_stop(USBVCamState *s)
{
    s->streaming = false;
    s->frame_pending = false;
    s->xfer_pos = 0;
    timer_del(s->frame_timer);
}

/* ---- USB callbacks ---- */

static void vcam_handle_reset(USBDevice *dev)
{
    USBVCamState *s = USB_VCAM(dev);

    vcam_stream_stop(s);
    s->seq = 0;
}

static void vcam_handle_control(USBDevice *dev, USBPacket *p, int request,
                                int value, int index, int length,
                                uint8_t *data)
{
    USBVCamState *s = USB_VCAM(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length,
                                  data);
    if (ret >= 0) {
        return;
    }

    switch (request) {
    case VendorDeviceRequest | VCAM_REQ_GET_INFO:
        if (length < VCAM_INFO_SIZE) {
            goto stall;
        }
        stw_le_p(data + 0, s->width);
        stw_le_p(data + 2, s->height);
        stl_le_p(data + 4, s->fourcc);
        stl_le_p(data + 8, s->fps);     /* fps_num */
        stl_le_p(data + 12, 1);         /* fps_den */
        p->actual_length = VCAM_INFO_SIZE;
        break;

    case VendorDeviceOutRequest | VCAM_REQ_SET_STREAM:
        if (value) {
            vcam_stream_start(s);
        } else {
            vcam_stream_stop(s);
        }
        break;

    case VendorDeviceOutRequest | VCAM_REQ_SET_FPS:
        if (value < 1 || value > VCAM_MAX_FPS) {
            goto stall;
        }
        s->fps = value;
        vcam_set_interval(s);
        break;

    default:
    stall:
        p->status = USB_RET_STALL;
        break;
    }
}

static void vcam_handle_data(USBDevice *dev, USBPacket *p)
{
    USBVCamState *s = USB_VCAM(dev);
    uint32_t chunk;

    if (p->pid != USB_TOKEN_IN || p->ep->nr != 1) {
        p->status = USB_RET_STALL;
        return;
    }

    if (!s->frame_pending) {
        /* nothing to send; xhci re-polls after usb_wakeup() */
        p->status = USB_RET_NAK;
        return;
    }

    chunk = MIN(p->iov.size, s->xfer_len - s->xfer_pos);
    usb_packet_copy(p, s->xfer_buf + s->xfer_pos, chunk);
    s->xfer_pos += chunk;
    if (s->xfer_pos == s->xfer_len) {
        s->frame_pending = false;
        s->xfer_pos = 0;
    }
}

/* ---- lifecycle ---- */

static void vcam_realize(USBDevice *dev, Error **errp)
{
    USBVCamState *s = USB_VCAM(dev);
    g_autoptr(GError) error = NULL;

    if (!s->format || g_ascii_strcasecmp(s->format, "grey") == 0) {
        s->fourcc = VCAM_FMT_GREY;
        s->payload_size = s->width * s->height;
    } else if (g_ascii_strcasecmp(s->format, "yuyv") == 0) {
        s->fourcc = VCAM_FMT_YUYV;
        s->payload_size = s->width * s->height * 2;
    } else {
        error_setg(errp, "format must be 'grey' or 'yuyv'");
        return;
    }
    if (s->width < VCAM_MIN_DIM || s->width > VCAM_MAX_DIM ||
        s->height < VCAM_MIN_DIM || s->height > VCAM_MAX_DIM ||
        (s->fourcc == VCAM_FMT_YUYV && (s->width & 1))) {
        error_setg(errp, "invalid frame size %ux%u", s->width, s->height);
        return;
    }
    if (s->fps < 1 || s->fps > VCAM_MAX_FPS) {
        error_setg(errp, "fps must be 1..%d", VCAM_MAX_FPS);
        return;
    }
    vcam_set_interval(s);

    if (s->frames) {
        size_t size;

        s->frame_file = g_mapped_file_new(s->frames, false, &error);
        if (!s->frame_file) {
            error_setg(errp, "cannot open frames '%s': %s",
                       s->frames, error->message);
            return;
        }
        size = g_mapped_file_get_length(s->frame_file);
        if (!size || size % s->payload_size) {
            error_setg(errp, "frames file must contain a nonzero multiple of %u bytes",
                       s->payload_size);
            g_mapped_file_unref(s->frame_file);
            s->frame_file = NULL;
            return;
        }
        s->frame_count = size / s->payload_size;
    }

    usb_desc_create_serial(dev);
    usb_desc_init(dev);
    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, 1);
    s->xfer_buf = g_malloc(VCAM_FRAME_HDR_SIZE + s->payload_size);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vcam_frame_timer, s);
}

static void vcam_unrealize(USBDevice *dev)
{
    USBVCamState *s = USB_VCAM(dev);

    if (s->frame_timer) {
        timer_del(s->frame_timer);
        timer_free(s->frame_timer);
    }
    g_free(s->xfer_buf);
    if (s->frame_file) {
        g_mapped_file_unref(s->frame_file);
    }
}

static const VMStateDescription vmstate_usb_vcam = {
    .name = TYPE_USB_VCAM,
    .unmigratable = 1,
};

static const Property vcam_properties[] = {
    DEFINE_PROP_UINT32("width",  USBVCamState, width, 640),
    DEFINE_PROP_UINT32("height", USBVCamState, height, 480),
    DEFINE_PROP_UINT32("fps",    USBVCamState, fps, 30),
    DEFINE_PROP_STRING("format", USBVCamState, format),
    DEFINE_PROP_STRING("frames", USBVCamState, frames),
};

static void vcam_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "QEMU Replay Camera";
    uc->usb_desc       = &desc_vcam;
    uc->realize        = vcam_realize;
    uc->unrealize      = vcam_unrealize;
    uc->handle_reset   = vcam_handle_reset;
    uc->handle_control = vcam_handle_control;
    uc->handle_data    = vcam_handle_data;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "QEMU Replay Camera (vendor bulk video source)";
    dc->vmsd = &vmstate_usb_vcam;
    device_class_set_props(dc, vcam_properties);
}

static const TypeInfo vcam_info = {
    .name          = TYPE_USB_VCAM,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBVCamState),
    .class_init    = vcam_class_init,
};

static void vcam_register_types(void)
{
    type_register_static(&vcam_info);
}

type_init(vcam_register_types)
