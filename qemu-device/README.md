# usb-vcam — QEMU USB replay-camera device model

A vendor-specific USB device that streams video frames on a bulk IN
endpoint at a fixed frame rate. The guest-side counterpart is the
`vcam_replay` kernel module (`../driver/`), which binds to it and exposes
`/dev/videoX`.

Currently the device generates a moving test pattern internally (diagonal
gradient + a vertical bar that advances one step per frame). That is enough
to bring up and validate the full stack — QEMU device model → xHCI → USB
core → `vcam_usb.c` → V4L2 → userspace — before adding a host-side image
feed (the intended extension point is `vcam_generate_pattern()` in
`dev-vcam.c`).

## Placing the file in the QEMU source tree

Tested against QEMU 11.0.1. Copy the device model into QEMU's USB
device directory:

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

should list `width`, `height`, `fps`, `format` properties.

## Running

Attach an xHCI controller and the camera to your guest:

```sh
qemu-system-aarch64 -M virt -cpu max -m 2G \
    ... kernel/disk options ... \
    -device qemu-xhci,id=xhci \
    -device usb-vcam,width=640,height=480,fps=30,format=grey
```

Properties:

| property | default | meaning                              |
|----------|---------|--------------------------------------|
| `width`  | 640     | frame width in pixels                |
| `height` | 480     | frame height in pixels               |
| `fps`    | 30      | frames per second (1–100000)         |
| `format` | grey    | `grey` (8-bit) or `yuyv` (packed YUV) |

Inside the guest, the device enumerates as `1209:000a QEMU Replay Camera`
(`lsusb`), and once `vcam_replay.ko` is loaded a `/dev/videoX` node appears.

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
