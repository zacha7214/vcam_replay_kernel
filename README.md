# vcam_replay — host images through an emulated USB camera

Replay GREY or YUYV images from the machine running QEMU into a Linux guest's
`/dev/videoN`. The custom USB device is `1209:000a`; the guest driver uses
V4L2 and videobuf2. This is a vendor USB protocol, not UVC or USB3 Vision.

```
Mac / QEMU host                         Linux guest
images → packed frames.raw → usb-vcam → xHCI → vcam_replay → /dev/videoN
                                               ↑
                               optional /dev/vcamN local feeder
```

## Build into your actual guest kernel

Run this in the Linux kernel build environment, using a **full source tree**
for the kernel you will boot. Python 3, GNU make, the kernel build dependencies
(including flex/bison), and a compiler for the guest architecture are needed.
No Ubuntu-specific paths, symbol addresses, or prebuilt `.ko` are used.

```sh
# Existing ARM64 configuration in /path/to/build/.config:
python3 scripts/setup-kernel.py /path/to/linux \
    --output /path/to/build --arch arm64 \
    --cross-compile aarch64-linux-gnu-

# Or a fresh build: explicitly choose the starting configuration.
python3 scripts/setup-kernel.py /path/to/linux \
    --output /path/to/build --arch arm64 \
    --cross-compile aarch64-linux-gnu- --defconfig defconfig

make -C /path/to/linux O=/path/to/build ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- -j4 Image
```

Omit `--output` for an in-source build. Omit `--cross-compile` for a native
build. For x86 use `--arch x86` and build `bzImage` instead of `Image`.
Use the same architecture, compiler and output directory for every command.
An existing `.config` is preserved as the starting point even if `--defconfig`
is supplied. A configured in-source tree may need a separate clean checkout
before switching to `O=`; the script does not clean your tree.

The installer:

- Copies the driver into `drivers/media/vcam-replay/` and adds Kconfig/Kbuild
  hooks without duplicating them on subsequent runs.
- Sets `CONFIG_VIDEO_VCAM_REPLAY=y`, media/camera/V4L2, USB, PCI/xHCI, I2C,
  devtmpfs and high-resolution timers. The driver's Kconfig selects the hidden
  videobuf2 V4L2/vmalloc/core/memops and DMA-buffer dependencies.
- Runs `olddefconfig` and fails unless **all 18 required symbols resolve to `y`**.
  Hidden options cannot reliably be enabled by simply writing `.config`.
- Backs up the replaced config, parent Kconfig/Makefile and previous installed
  driver under `/path/to/linux/.vcam-backups/<timestamp>/`. On failure, edits
  remain for inspection; restore those backups for rollback. On first install,
  rollback also requires removing the newly created `drivers/media/vcam-replay/`.

A read-only recheck detects stale installed sources as well as config problems:

```sh
python3 scripts/setup-kernel.py /path/to/linux --output /path/to/build --check
```

The script configures the camera path; keep your existing QEMU machine,
root filesystem, storage, console and PCI host-controller settings. It does
not deploy/boot the kernel. The optional `--rootfs-busybox` flag below adds
a minimal root filesystem. For ARM64 `virt`
with `qemu-xhci`, the guest also needs the machine's PCI host support (normally
present in ARM64 `defconfig`).

Copy the resulting `arch/arm64/boot/Image` to the Mac and use it as QEMU's
`-kernel`. Add **`vcam_replay.devices=0`** to your existing kernel command line
(`-append`) for USB only. Built-in drivers need no `insmod` or `modprobe`.
The default creates one extra local-feed camera, which can change video numbering.

Symbol failures in a separately loaded module can indicate mismatched builds,
missing dependencies or symbol-version mismatches; there were no hardcoded
V4L2 symbol addresses in this driver. A complete in-tree build resolves the
symbols together. For an external module, `modules_prepare` alone does not
produce the `Module.symvers` needed with module versioning; see the
[kernel build documentation](https://docs.kernel.org/kbuild/modules.html).

## Lightweight rootfs for a first Mac boot

`build-qemu.py` builds the **emulator**, not the Linux kernel or a rootfs.
`arch/arm64/boot/Image` is the kernel. An empty `usr/initramfs_data.cpio`
with `CONFIG_INITRAMFS_SOURCE=""` does not supply Linux userspace.

For a small boot/detection environment, use `scripts/build-rootfs.py`. It
packages a **static ARM64 Linux BusyBox** into a roughly 1 MiB compressed
initramfs. Packaging works on macOS or Linux using Python alone, with no
mounting, formatting, sudo, or kernel rebuild (if its config passes validation).
Use a trusted BusyBox build with `sh`, `init`, `mount`, `mkdir`, and `--install`
support; the Ubuntu `busybox-static` package in an ARM64 VM works. The script
rejects the wrong ELF architecture and dynamically linked binaries, checks
kernel boot options, and refuses to overwrite existing output.

From this repository on the Mac, using the existing Multipass VM:

```sh
# In the ARM64 VM, only if its BusyBox is not already static:
# multipass exec tito-domingo -- sudo apt install busybox-static
mkdir -p artifacts
multipass transfer tito-domingo:/bin/busybox artifacts/busybox-arm64
python3 scripts/build-rootfs.py \
    --busybox artifacts/busybox-arm64 \
    --kernel-config ../vcam_kernel_build/.config \
    --output artifacts/initramfs.cpio.gz
```

For future Linux kernel builds, add `--rootfs-busybox /bin/busybox` to
`setup-kernel.py ... --arch arm64`. This also enables/checks the ARM64 `virt`
console, PCI and initramfs built-ins and writes `initramfs.cpio.gz` alongside
the output `.config`. Then build `Image` as usual and copy **both** files to
the Mac. This is a separate initramfs passed at launch, not embedded in `Image`.
An existing archive must be moved aside before generating its replacement.

With the paths used on this Mac:

```sh
python3 tools/vcam_images.py pattern /tmp/local-vcam.raw
python3 scripts/run-qemu.py \
    --qemu ~/vcam-qemu-project/qemu-vcam/build-vcam/qemu-system-aarch64 \
    --kernel ~/vcam-qemu-project/vcam_kernel_build/arch/arm64/boot/Image \
    --initrd artifacts/initramfs.cpio.gz \
    --append 'rdinit=/init' \
    --frames /tmp/local-vcam.raw
```

Press Enter to open the serial root shell, then check:

```sh
dmesg | grep -E 'vcam|xhci'
ls -l /dev/video*
cat /sys/class/video4linux/video0/name
```

This environment is RAM-only; changes disappear on shutdown. It has no SSH
server, Python, or `v4l2-ctl`, so omit `--ssh-port` and use it for boot and
camera detection. `poweroff -f` shuts down; Ctrl-a then x exits QEMU. Use TCG
(the default) for the initial check. The packaged kernel, Mac emulator and
this BusyBox initramfs have been boot-tested together with TCG; `/dev/video0`
is detected. Frame capture is a separate check requiring capture tools.

For a persistent full guest with SSH and capture utilities, use a populated
ARM64 Linux disk image. **qcow2 is the disk container, not the guest filesystem.**
Creating an empty qcow2 does not install Linux. A Linux-prepared ext4 filesystem
containing userspace can occupy the whole disk (`root=/dev/vda rw rootwait`),
or a partition (`/dev/vda1`, `/dev/vda2`, etc., according to its actual layout).
A whole-filesystem raw ext4 image can be converted with
`qemu-img convert -f raw -O qcow2 rootfs.ext4 rootfs.qcow2`; conversion preserves
its unpartitioned layout, so it still uses `/dev/vda`, not `/dev/vda2`.
Do Linux filesystem provisioning in the VM; do not format it APFS/exFAT on the
Mac. Direct `-kernel` boot needs no EFI partition or bootloader. The kernel must
have the disk/filesystem drivers built in, or use a matching initramfs.
Install/configure SSH and networking in that guest before using `--ssh-port`.
Use `--writable-disk` to retain disk changes; the default is a temporary snapshot.

## Prepare images on the Mac / QEMU host

For a directory of JPEGs, use the optional Pillow-based converter:

```sh
# Pillow is already available in the current Mac Python. If needed elsewhere:
python3 -m venv .venv
.venv/bin/python -m pip install Pillow
.venv/bin/python tools/vcam_jpegs.py /path/to/jpgs --output /tmp/vcam-jpegs.raw
```

With Pillow already installed, simply use `python3 tools/vcam_jpegs.py ...`.
The default is 640×480 GREY at a suggested 30 fps. `.jpg` and `.jpeg` extensions
are case-insensitive; only the selected directory is scanned. Files are sorted
by filename (zero-pad frame numbers). EXIF orientation is applied, then each
image is converted to grayscale and resized with black padding to preserve
its aspect ratio. Color is not retained. `--width`, `--height` and `--fps`
change the geometry and printed launch options. Without `--output`, a unique
`/tmp/vcam-frames-*.raw` name is chosen. Existing raw/manifest files are never
overwritten, and decode failures remove partial output.

The script prints the exact `run-qemu.py` options, for example:

```sh
--frames /tmp/vcam-jpegs.raw --width 640 --height 480 --format grey --fps 30
```

It also writes a `.raw.json` manifest compatible with `vcam_images.py verify`.
The frame sequence loops in QEMU; do not modify it while QEMU is running.
For a persistent guest with SSH and capture tools, see the complete
[ARM64 mmdebstrap build and launch recipe](docs/mmdebstrap.md).


Python 3 is the only dependency of the host tool. Start with a single distinctive
frame for an unambiguous pixel comparison:

```sh
python3 tools/vcam_images.py pattern /tmp/vcam-smoke.raw
# Writes /tmp/vcam-smoke.raw and /tmp/vcam-smoke.raw.json.
```

Or pack a lexicographically sorted sequence (zero-pad numerical filenames):

```sh
python3 tools/vcam_images.py pack /path/to/pgms /tmp/camera.raw
python3 tools/vcam_images.py pack /path/to/raws /tmp/camera-yuyv.raw \
    --width 640 --height 480 --format yuyv
```

PGM inputs must be binary P5, 8-bit (`maxval=255`) with identical dimensions.
Each `.raw` input must contain exactly one tightly packed frame. The output
contains only concatenated pixels; geometry/format are supplied to QEMU.
The adjacent JSON file records dimensions and source frame hashes for checking.
Existing outputs are refused. Do not modify/truncate a frames file while QEMU
has it mapped. This is file replay; a live socket/network feeder is not included.

## Build and run locally on Linux

The same machine can build the kernel, run QEMU and supply images. No transfer
or `--local` switch is needed: the launcher always uses local paths. These
scripts target an **ARM64 Linux guest**, on either an ARM64 or x86 Linux host.
They also support Apple silicon macOS as described below.

`~/qemu-11.1.1` on this machine has already been set up with `dev-vcam.c`,
`CONFIG_USB_VCAM` and the Meson source entry. The previous config files were
saved under `~/qemu-11.1.1/.vcam-backups/`. To repeat setup after changing the
model, or set up another QEMU tree, run from this repo:

```sh
python3 scripts/setup-qemu.py ~/qemu-11.1.1
python3 scripts/setup-qemu.py ~/qemu-11.1.1 --check
```

On Ubuntu/Debian install these dependencies before building (none are installed
by our scripts):

```sh
sudo apt update
sudo apt install build-essential ninja-build pkg-config \
    python3 python3-venv python3-pip python3-setuptools python3-wheel meson \
    libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev zlib1g-dev
```

This is for the headless configuration used here: GLib, pixman, device-tree
support, and user-mode networking. It does not require GTK/SDL, Rust, Sphinx,
or USB passthrough libraries. QEMU manages its build Python virtual environment;
this source release includes Meson/pycotap wheels. See the
[QEMU build environment documentation](https://www.qemu.org/docs/master/devel/build-environment.html)
for other platforms and optional features.

Build it yourself:

```sh
python3 scripts/build-qemu.py --source ~/qemu-11.1.1 -j 4
```

The script configures `~/qemu-11.1.1/build-vcam`, builds only
`qemu-system-aarch64`, and checks that `usb-vcam,help` exposes `frames`.
It does not install QEMU system-wide. `--build-dir PATH` selects another build
directory, and `--dry-run` checks integration and prints commands without
configuring/building. Default job count is capped at four to limit memory use.
It retains QEMU's default emulated devices while disabling unneeded optional
host features, then explicitly enables TCG, networking and device-tree support.
On ARM64 Linux it also compiles KVM support; runtime use of KVM is optional.

Prepare images with `tools/vcam_images.py` as above, then launch with your
built kernel and **existing guest root filesystem**:

```sh
python3 tools/vcam_images.py pattern /tmp/local-vcam.raw

python3 scripts/run-qemu.py \
    --kernel /path/to/build/arch/arm64/boot/Image \
    --disk /path/to/guest.qcow2 --disk-format qcow2 \
    --append 'root=/dev/vda2 rw' \
    --frames /tmp/local-vcam.raw --ssh-port 2222
```

Replace `root=/dev/vda2` with the partition/device used by your image; a raw
whole-filesystem disk may instead need `root=/dev/vda` and `--disk-format raw`.
The script requires explicit root arguments when using a disk, because it
cannot determine the guest partition layout. It supplies `console=ttyAMA0`
and `vcam_replay.devices=0` itself. The rootfs/initramfs must contain your
userspace tools, and the kernel needs the normal ARM64 `virt` PCI, virtio block
and network support in addition to the camera options. Existing working guest
images may also require an initramfs:

```sh
python3 scripts/run-qemu.py \
    --kernel /path/to/Image --initrd /path/to/initramfs.cpio.gz \
    --append 'rdinit=/init' --frames /tmp/local-vcam.raw
```

`--disk` and `--initrd` can be used together. The launcher does not create or
install an operating system. Use `--dry-run` to inspect its complete command
before QEMU is built. Other options include `--qemu /path/to/qemu-system-aarch64`,
`--memory 4G`, `--cpus 4`, `--width`, `--height`, `--format`, and `--fps`.
Omit `--frames` for the generated pattern.

The default accelerator is **TCG**, which works without `/dev/kvm`. This host
currently has no `/dev/kvm`. On an ARM64 Linux host with accessible KVM use
`--accel kvm`; on Apple silicon use `--accel hvf` with a QEMU build that enables
HVF. QEMU and the guest still run on the same machine in all these cases.

Disk writes are temporary by default (`-snapshot`). Add `--writable-disk` if
you want package installation or other guest changes to survive shutdown.
`--ssh-port 2222` forwards only `127.0.0.1:2222` to guest port 22; the guest
must run SSH and have networking configured. For example, copy a capture back
with `scp -P 2222 guestuser@127.0.0.1:/tmp/captured.raw /tmp/captured.raw` and
verify it locally with `tools/vcam_images.py`. The serial console uses the
terminal; QEMU's default exit sequence is Ctrl-a followed by x.

Inside the guest, use the same capture and verification procedure below.
The launcher leaves QEMU attached to your terminal for interactive use.

## Launch QEMU on the Mac

Keep your existing working device integration. **Re-copy the updated
`qemu-device/dev-vcam.c` and rebuild QEMU once** to add the `frames` property;
[the QEMU notes](qemu-device/README.md) describe the two existing build hooks.
Confirm `-device usb-vcam,help` lists `frames`.

Add to your usual working QEMU command:

```sh
-device qemu-xhci,id=xhci \
-device usb-vcam,bus=xhci.0,width=640,height=480,fps=30,format=grey,frames=/tmp/vcam-smoke.raw
```

`frames=` is an absolute path on the **QEMU host**, not inside the guest. The
file loops at the device's virtual-clock frame rate and rewinds on each
STREAMON. Omit it for the moving test pattern. Match width/height/format to
those printed by the packing tool. With paths containing spaces, quote the
whole device argument; QEMU option parsing requires literal commas in a path
to be escaped as doubled commas.

## Capture and check inside the guest

Install Python 3 and `v4l2-ctl` (usually the `v4l-utils` package) in your existing
guest. Copy this repo and the small `.raw.json` manifest into it, or copy the
capture back to the host for verification there.

```sh
dmesg | grep -E 'vcam|xhci'
lsusb                         # expect 1209:000a
v4l2-ctl --list-devices        # expect vcam-usb

python3 scripts/capture-guest.py /tmp/captured.raw \
    --manifest /path/to/vcam-smoke.raw.json
# Repeat to check stream shutdown/restart as well:
python3 scripts/capture-guest.py /tmp/captured-again.raw \
    --manifest /path/to/vcam-smoke.raw.json
```

The helper discovers `vcam-usb` by its sysfs name, captures 30 mmap frames,
and fails after 20 seconds instead of hanging indefinitely. Use `--device`,
`--count`, or `--timeout` to override. Run with permission to open the video
node (video-group membership or root). A timeout/error leaves a partial capture
for diagnosis; use a new output name when retrying.

Without the helper:

```sh
v4l2-ctl -d /dev/videoN --stream-mmap=4 --stream-count=30 --stream-to=/tmp/captured.raw
python3 tools/vcam_images.py verify /path/to/vcam-smoke.raw.json /tmp/captured.raw
```

Success means each captured frame matches a source image byte-for-byte.
For a sequence, default verification permits dropped/reordered/repeated frames;
`--strict-sequence` requires the first source image first, then exact looping
order with no drops. The capture path uses latest-frame buffering and can drop
frames under load. Start at 30 fps; 2000 fps is not a validated throughput claim.
The core's V4L2 sequence numbers count delivery attempts, not USB wire sequence
numbers, so they do not expose every transport/source drop.

If enumeration works but capture does not, inspect `dmesg`, confirm you booted
the rebuilt kernel, and use the generated pattern without `frames=` to isolate
the file input. Missing `/dev/videoN` with a sysfs node present indicates device
node management (devtmpfs/udev); initramfs users may need to mount devtmpfs themselves.

## Local feed and external module development

The character-device frontend is useful for testing V4L2 without QEMU. With
`vcam_replay.devices=1` on the kernel command line it provides `/dev/vcam0`.
`tools/vcam_feed` runs **inside Linux**, not on macOS; it writes to this local
node. See `tools/vcam_feed -h` after `make -C tools`.

For optional external-module testing against the exact configured/built guest tree:

```sh
make -C driver KDIR=/path/to/build ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
# In the matching guest, only if the driver is not already built in:
sudo modprobe videobuf2_vmalloc
sudo modprobe videobuf2_v4l2
sudo insmod vcam_replay.ko devices=0
```

## Review fixes

The driver now uses target-header feature checks for newer hrtimer and vb2
APIs, includes the V4L2 file-handle declarations explicitly, and avoids the
removed `no_llseek` helper. Other fixes cover the missing name on the
parentless local camera, keeping the device alive until teardown finishes,
rolling back format metadata if allocation fails, propagating USB startup
errors to STREAMON, waking capture waiters on a failed URB resubmission,
and validating USB geometry before allocation. Malformed USB headers now
resynchronize instead of trusting arbitrarily large skip lengths.

## Validation

```sh
python3 -m unittest discover -s tests -v
make -C tools
```

The tests exercise image packing/corruption detection, installer repeatability
and failure checks, and the actual USB parser in a small userspace C harness
(fragmentation, coalesced frames, junk and malformed lengths). They require
Python 3 and a native C compiler. Installer unit tests simulate Kconfig; real
kernel integration was additionally checked with `olddefconfig` and a complete
ARM64 `vmlinux` link using a disposable Linux 7.3-rc4 tree starting from
`allnoconfig`. That minimal configuration was a link test, not a bootable guest
recipe. The external module also compiled and passed modpost against installed
Linux 7.0 headers. Header feature checks accommodate timer/videobuf2 API changes;
other kernel versions still need a build check with their own source tree.

The Mac QEMU binary and packaged ARM64 kernel have also booted successfully
with the minimal BusyBox initramfs, registering `vcam-usb` as `/dev/video0`.
Live frame capture remains unverified; use a guest with the capture tools
to complete the end-to-end check. The Linux UAPI parser harness is skipped
on macOS; the Python packaging/launcher tests run on both platforms.
