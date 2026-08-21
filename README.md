# vcam_replay — simulated USB camera video replay

Replays image sequences as V4L2 video at a controllable frame rate inside a
Linux guest, delivered as if from a real USB camera. See `CLAUDE.md` for the
full project brief.

```
Phase 1 (no USB):   tools/vcam_feed ──write()──► /dev/vcam0 ─┐
                                                             ├─► vcam core
Phase 2 (USB):      QEMU usb-vcam ──xHCI──► vcam_usb.c ──────┘   (vb2 + hrtimer)
                                                             │
                                                             ▼
                                                        /dev/videoX
                                                (v4l2-ctl / ffplay / aravis…)
```

## Layout

| path                     | contents                                                            |
|--------------------------|---------------------------------------------------------------------|
| `driver/`                | guest kernel module `vcam_replay.ko`                                |
| `driver/vcam_core.c`     | V4L2 device + videobuf2 + hrtimer frame pacing (shared engine)      |
| `driver/vcam_chardev.c`  | Phase 1 frontend: `/dev/vcamN` control node fed from userspace      |
| `driver/vcam_usb.c`      | Phase 2 frontend: binds to the QEMU `usb-vcam` device               |
| `driver/vcam_uapi.h`     | ioctl ABI + USB wire protocol (single source of truth)              |
| `tools/vcam_feed.c`      | host-side feeder: pushes PGM/raw frames at a paced rate             |
| `qemu-device/`           | QEMU device model + integration instructions (see its README)      |

## Design notes

- **One core, two frontends.** Unlike v4l2loopback (which is a V4L2
  output→capture pipe), the core here takes frames through a plain kernel
  API (`vcam_submit_frame()`), so a USB transport slots in without touching
  the V4L2 side — that's the "set up slightly differently than
  v4l2loopback" requirement.
- **Timing.** In self-paced mode (chardev frontend) an `hrtimer` in
  `HRTIMER_MODE_REL` with `hrtimer_forward_now()` delivers the most recent
  frame every period — drift-free, jitter limited to kthread wakeup latency.
  In source-paced mode (USB frontend) the emulated camera paces the stream
  with a QEMU virtual-clock timer and every received frame is delivered
  immediately, like a real UVC camera.
- **The producer owns the format.** V4L2 clients see a fixed-format camera:
  `S_FMT` coerces to whatever the feeder/device configured, so pushed
  frames can never mismatch allocated buffers.
- **Frame path is lock-light.** Producer and delivery thread exchange
  frames through a triple buffer; only pointer swaps happen under the
  spinlock, never full-frame copies.

## Phase 1 — chardev feed (no USB)

Build and load the module in the guest (or on any Linux box — the video
path is independent of QEMU):

```sh
cd driver && make            # builds against the running kernel
sudo insmod vcam_replay.ko fps=30 width=640 height=480 format=GREY
# dmesg: vcam0: registered video0, 640x480 GREY @ 30/1 fps (self-paced)
```

Streaming works immediately (a built-in gradient pattern is replayed until
the first real frame arrives), so timing can be validated with no feeder:

```sh
v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=300  # prints actual fps
ffplay -f v4l2 /dev/video0
```

Feed real frames (e.g. Basler exports as binary PGM, mounted into the guest
via virtfs):

```sh
# host: -virtfs local,path=/host/images,mount_tag=host0,security_model=mapped
# guest:
mount -t 9p -o trans=virtio host0 /mnt/images
cd tools && make
./vcam_feed -r 30 -l -s /mnt/images        # PGM is self-sizing
./vcam_feed -r 2000 -w 640 -H 480 -f GREY -l /mnt/raw   # headerless .raw
```

Rate control at runtime: `v4l2-ctl --set-parm=60`, the feeder's `-r`, or
the `VCAM_IOC_S_FPS` ioctl. Timing accuracy: compare `v4l2-ctl`'s measured
fps against the target, and check `ticks_no_buffer` / sequence gaps via
`VCAM_IOC_G_STATS` (the feeder's `-s` flag prints them once per second).

## Phase 2 — over emulated USB

1. Build QEMU with the `usb-vcam` device — see `qemu-device/README.md`.
2. Run the guest with:
   `-device qemu-xhci -device usb-vcam,width=640,height=480,fps=30,format=grey`
3. In the guest: `insmod vcam_replay.ko` (the same module registers the USB
   driver; `devices=0` skips the chardev instance if you only want USB).
4. The device enumerates as `1209:000a`, the driver probes it, reads the
   format via the `GET_INFO` control request, and registers a second
   `/dev/videoX`. `STREAMON` sends `SET_STREAM 1` to the device, which then
   emits one header+payload record per frame period on the bulk pipe.

```sh
v4l2-ctl -d /dev/video1 --stream-mmap --stream-count=300
ffplay -f v4l2 /dev/video1     # moving test pattern at the configured fps
```

## Roadmap to the end goal

- Feed real Basler frames into `usb-vcam` from the host (replace
  `vcam_generate_pattern()`; add a `path=`/socket property).
- Push the rate toward 2000 fps (bulk pipe and protocol already carry
  explicit sequence numbers so drops are measurable).
- GenICam/USB3 Vision layer for Aravis compatibility (descriptor + protocol
  work on top of the same transport).
