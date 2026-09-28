# usb-vcam — QEMU USB replay-camera device model

A vendor-specific USB device that streams video frames on a bulk IN
endpoint at a fixed frame rate. The guest-side counterpart is the
`vcam_replay` kernel module (`../driver/`), which binds to it and exposes
`/dev/videoX`.

The device generates a moving test pattern by default. With
`frames=/absolute/host/path/frames.raw`, it replays a memory-mapped file of
concatenated raw frames in a loop. The file lives on the QEMU host (including
macOS), not in the guest. Prepare it with `tools/vcam_images.py`; see the
[main README](../README.md) for the complete built-in kernel and pixel-check
workflow. Rebuild QEMU with this updated source to obtain the `frames` property.
Do not modify or truncate the file while QEMU is running.

## Automated setup and local Linux build

From the parent project, use:

```sh
python3 scripts/setup-qemu.py ~/qemu-11.1.1
python3 scripts/build-qemu.py --source ~/qemu-11.1.1 -j 4
```

The first command copies the model and adds the Kconfig/Meson entries below,
with backups and repeat-run checks. The second builds into `build-vcam/` and
checks the device properties. Setup has already been applied to
`/home/ubuntu/qemu-11.1.1`; the build has not been run. Install the dependencies
listed in the [local Linux workflow](../README.md#build-and-run-locally-on-linux)
first. That section also covers `scripts/run-qemu.py`, which launches using
local kernel, disk/initramfs and image paths on Linux or macOS.

The manual integration below is equivalent to the setup script.

## Placing the file in the QEMU source tree

The original device targeted QEMU 11.0.1. The new file-replay extension has
not been compiled/run against QEMU in this environment. Copy the device model
into QEMU's USB device directory:

```sh
cp dev-vcam.c /path/to/qemu/hw/usb/dev-vcam.c
```

## Build config changes (two edits)

**1. `hw/usb/Kconfig`** — add a config symbol (anywhere among the other
device entries, e.g. after `USB_CANOKEY`):

```
config USB_VCAM
    bool
    default y
    depends on USB
```

`default y` + `depends on USB` means the device is built into every target
whose machines pull in USB support — for arm64 the `virt` machine gets USB
via `USB_XHCI_PCI` (`default y if PCI_DEVICES`), so no per-machine or
`configs/devices/...` changes are needed.

**2. `hw/usb/meson.build`** — register the source file in the
"emulated usb devices" block:

```meson
system_ss.add(when: 'CONFIG_USB_VCAM', if_true: files('dev-vcam.c'))
```

## Building QEMU (arm64 system emulator)

```sh
cd /path/to/qemu
mkdir -p build && cd build
../configure --target-list=aarch64-softmmu
ninja qemu-system-aarch64
```

On a macOS host, `brew install ninja pkg-config glib pixman meson` first;
add `--enable-hvf` if you want hardware acceleration for arm64 guests on
Apple silicon.

Sanity check that the device got in:

```sh
./qemu-system-aarch64 -device usb-vcam,help
```

should list `width`, `height`, `fps`, `format`, and `frames` properties.

## Running

Attach an xHCI controller and the camera to your guest:

```sh
qemu-system-aarch64 -M virt -cpu max -m 2G \
    ... kernel/disk options ... \
    -device qemu-xhci,id=xhci \
    -device usb-vcam,bus=xhci.0,width=640,height=480,fps=30,format=grey,frames=/tmp/camera.raw
```

Properties:

| property | default | meaning                              |
|----------|---------|--------------------------------------|
| `width`  | 640     | frame width in pixels                |
| `height` | 480     | frame height in pixels               |
| `fps`    | 30      | frames per second (1–100000)         |
| `format` | grey    | `grey` (8-bit) or `yuyv` (packed YUV) |
| `frames` | unset   | host raw-frame file; unset generates a pattern |

Inside the guest, the device enumerates as `1209:000a QEMU Replay Camera`
(`lsusb`). The built-in `vcam_replay` driver binds automatically and creates
`/dev/videoX`; add `vcam_replay.devices=0` to the kernel command line to skip
the extra local-feed camera. An external `.ko` can also be used with a matching
kernel that does not already contain the driver.

## Device model internals / protocol

- **Enumeration**: VID:PID `0x1209:0x000a`, one configuration, one
  vendor-class (0xff) interface, one bulk IN endpoint (0x81). Full- and
  high-speed descriptors are provided; behind xHCI it runs high-speed.
- **Vendor control requests** (device-scope):
  - `GET_INFO` (0x01, IN): 16 bytes LE — width u16, height u16, fourcc u32,
    fps_num u32, fps_den u32. The guest driver reads this at probe time.
  - `SET_STREAM` (0x02, OUT): wValue 1 starts the internal frame timer,
    0 stops it. The guest driver ties this to V4L2 STREAMON/STREAMOFF.
  - `SET_FPS` (0x03, OUT): wValue = new integer frame rate.
- **Streaming**: a `QEMU_CLOCK_VIRTUAL` timer fires once per frame period
  (kept drift-free by advancing the deadline on its own timeline). Each
  tick produces one record on the bulk pipe: a 32-byte header (magic
  `"VCAM"`, sequence number, geometry, fourcc, payload length) followed by
  the raw payload. When no data is pending the endpoint NAKs, and
  `usb_wakeup()` re-arms the xHCI when the next frame is ready — the guest
  just keeps a bulk URB in flight, exactly as it would with real hardware.
- **Overrun behavior**: if the guest is still mid-transfer when the next
  tick arrives, the new frame is dropped (sequence number still advances,
  so the guest can detect gaps); an untouched pending frame is overwritten
  ("latest frame wins", like a free-running sensor).

The wire-protocol constants are duplicated from `../driver/vcam_uapi.h` —
keep the two in sync if you change the protocol.

The frames file must be nonempty and an exact multiple of the configured
frame size. GREY uses width*height bytes, YUYV uses width*height*2 bytes
(and requires even width). There is no PGM header on this input. STREAMON
rewinds to frame zero; the file loops indefinitely. Slow guests may skip
source frames when an untouched pending frame is replaced. Live migration
is disabled for this device.
