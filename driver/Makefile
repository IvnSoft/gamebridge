# SPDX-License-Identifier: GPL-2.0
# Out-of-tree build for the GameBridge driver.
#
#   make                build gamebridge.ko against the running kernel
#   make KDIR=/path     build against a different kernel's headers
#   make clean
#   sudo make install   install the module and run depmod
#   sudo make firmware  install the extracted 8051 image (NOT redistributable)
#
# Fedora ships the headers in kernel-devel. The mainline media sources are NOT
# in that package -- drivers/media/*/ contains a Kconfig and a Makefile and
# nothing else.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

FW_SRC ?= ../firmware/gb-loader-fw.bin
FW_DST ?= /lib/firmware/gamebridge/gb-loader-fw.bin
# The image tools/extract-fw.py produces from the pinned avcgbfl.sys; checked
# before install, since the kernel loader can check only size and first byte.
FW_SHA256 := 39845e82eb7691dddcea1e0ec55c53881dba200437fb0e369e53439d5260b22f

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

# The image is extracted from a copyrighted Adaptec binary.
# Build it first with tools/fetch-firmware.sh and tools/extract-fw.py.
firmware:
	@test -f $(FW_SRC) || { \
		echo "$(FW_SRC) missing. Build it with:"; \
		echo "  ./tools/fetch-firmware.sh"; \
		echo "  python3 tools/extract-fw.py firmware/avcgbfl.sys $(FW_SRC)"; \
		exit 1; }
	@echo "$(FW_SHA256)  $(FW_SRC)" | sha256sum -c --quiet - || { \
		echo "$(FW_SRC) is not the pinned image; re-run tools/extract-fw.py"; \
		exit 1; }
	install -Dm644 $(FW_SRC) $(FW_DST)
	@echo "installed $(FW_DST)"

.PHONY: all clean install firmware
