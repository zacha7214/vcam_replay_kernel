# ARM64 Debian rootfs for the Mac QEMU launcher

Run the build commands **inside the ARM64 Linux VM**, not on macOS. They create
an ordinary directory first so you can configure a login and networking before
packing it into a disk. Use fresh output names. This recipe is supplied for you
to execute; it has not been built/boot-tested here.

```sh
sudo apt update
sudo apt install mmdebstrap debian-archive-keyring e2fsprogs

sudo mmdebstrap --architectures=arm64 --variant=minbase --format=directory \
  --keyring=/usr/share/keyrings/debian-archive-keyring.gpg \
  --include=systemd-sysv,udev,systemd-resolved,openssh-server,python3,v4l-utils,usbutils,iproute2,ca-certificates \
  trixie ./vcam-rootfs http://deb.debian.org/debian

# Create the login used on the serial console and over SSH.
sudo chroot ./vcam-rootfs useradd --create-home --shell /bin/bash --groups video vcam
sudo chroot ./vcam-rootfs passwd vcam

# Configure DHCP regardless of whether the virtual NIC is named eth0 or enp0s2.
sudo tee ./vcam-rootfs/etc/systemd/network/20-wired.network >/dev/null <<'EOF'
[Match]
Name=en* eth*
[Network]
DHCP=yes
EOF
sudo ln -sf /run/systemd/resolve/stub-resolv.conf ./vcam-rootfs/etc/resolv.conf
sudo systemctl --root="$PWD/vcam-rootfs" enable \
  systemd-networkd.service systemd-resolved.service ssh.service serial-getty@ttyAMA0.service

# A whole-filesystem ext4 image: no partition table, EFI, or bootloader needed.
# Exclusive creation protects an existing image. Keep ownership under this user.
python3 -c 'with open("vcam-rootfs.ext4", "xb") as f: f.truncate(2 * 1024**3)'
sudo mkfs.ext4 -F -L vcam-root -d ./vcam-rootfs ./vcam-rootfs.ext4
```

The interactive `passwd` step sets your chosen password; none is hardcoded.
The `vcam` user can capture via the `video` group but is not a sudo/admin user.
Use the VM's `sudo chroot ./vcam-rootfs ...` before packaging to install other
packages or configure administrative access. No separate guest kernel package
is needed: QEMU loads your existing ARM64 `Image`. The camera, virtio block,
and ext4 drivers are already built into that kernel.

The file is a sparse 2 GiB image; copying it may use the full logical size.
From the **Mac**, assuming the VM build ran in `/home/ubuntu`:

```sh
multipass transfer tito-domingo:/home/ubuntu/vcam-rootfs.ext4 \
  ~/vcam-qemu-project/vcam-rootfs.ext4

python3 scripts/run-qemu.py \
  --qemu ~/vcam-qemu-project/qemu-vcam/build-vcam/qemu-system-aarch64 \
  --kernel ~/vcam-qemu-project/vcam_kernel_build/arch/arm64/boot/Image \
  --disk ~/vcam-qemu-project/vcam-rootfs.ext4 --disk-format raw \
  --append 'root=/dev/vda rootfstype=ext4 rw rootwait' \
  --frames /tmp/vcam-jpegs.raw --width 640 --height 480 --format grey --fps 30 \
  --writable-disk --ssh-port 2222
```

Use the JPEG packer's actual output path if different. Do **not** pass the
minimal BusyBox `--initrd` or `rdinit=/init`: that initramfs stays in its own
RAM root rather than switching to this disk. `--writable-disk` persists guest
changes. `root=/dev/vda` is correct for the unpartitioned image created above.
The previously validated BusyBox boot does not validate this Debian recipe.

After boot, log in as `vcam` on the serial console or from the Mac:

```sh
ssh -p 2222 vcam@127.0.0.1
```

Inside the guest:

```sh
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=30 --stream-to=/tmp/captured.raw
```

Back on the Mac:

```sh
scp -P 2222 vcam@127.0.0.1:/tmp/captured.raw /tmp/vcam-captured.raw
python3 tools/vcam_images.py verify /tmp/vcam-jpegs.raw.json /tmp/vcam-captured.raw
```

References: [mmdebstrap options and formats](https://manpages.debian.org/trixie/mmdebstrap/mmdebstrap.1.en.html),
[systemd network configuration](https://manpages.debian.org/trixie/systemd/systemd.network.5.en.html).
