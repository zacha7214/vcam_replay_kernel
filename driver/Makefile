# Out-of-tree build for the vcam_replay module.
#
#   make            - build against the running kernel's headers
#   make KDIR=...   - build against a specific kernel tree (e.g. the
#                     guest kernel when cross-building for arm64:
#                     make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#                          KDIR=/path/to/guest/kernel)

KDIR ?= /lib/modules/$(shell uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
