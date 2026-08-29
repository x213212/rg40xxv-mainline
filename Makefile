# RG40XX V mainline port — build entry point.
#
#   make deps      what you need installed
#   make fetch     download and verify the pristine Linux 7.2 tarball
#   make tree      unpack it and apply the port's patches
#   make config    apply the production kernel configuration
#   make kernel    build Image + device trees          (this is the long one)
#   make dtbs      build device trees only             (fast, checks the DTS work)
#   make artifacts collect Image and the RG40XX V dtbs into out/
#   make all       fetch -> tree -> config -> kernel -> artifacts
#   make clean     remove the build tree, keep the download
#
# Flashing is deliberately not a target here: it writes to a physical card and
# wants a human reading docs/flash.md. See `make flash-help`.

KVER          ?= 7.2
KTARBALL      := linux-$(KVER).tar.xz
KURL          ?= https://cdn.kernel.org/pub/linux/kernel/v7.x/$(KTARBALL)
DOWNLOADS     ?= downloads
TREE          ?= build/linux-$(KVER)
OUT           ?= out
ARCH          ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
JOBS          ?= $(shell nproc 2>/dev/null || echo 4)
DEFCONFIG     := configs/rg40xxv_production_defconfig
PATCHES       := $(sort $(wildcard patches/*.patch))
DTB_DIR       := arch/arm64/boot/dts/allwinner
DTBS          := sun50i-h700-anbernic-rg40xx-v.dtb sun50i-h700-anbernic-rg40xx-v-v2-panel.dtb

MAKEKERNEL = $(MAKE) -C $(TREE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -j$(JOBS)

.PHONY: all deps fetch tree config kernel dtbs artifacts clean distclean flash-help help

all: artifacts

help:
	@sed -n '2,20p' Makefile | sed 's/^# \{0,1\}//'

deps:
	@echo "Required: gcc-aarch64-linux-gnu, make, bc, bison, flex, libssl-dev,"
	@echo "          libelf-dev, xz-utils, curl, device-tree-compiler"
	@printf 'cross gcc: '; $(CROSS_COMPILE)gcc --version 2>/dev/null | head -1 || echo "MISSING"

$(DOWNLOADS)/$(KTARBALL):
	@mkdir -p $(DOWNLOADS)
	curl -fL --retry 3 -o $@.part $(KURL)
	@mv $@.part $@

fetch: $(DOWNLOADS)/$(KTARBALL)
	@echo "have $(DOWNLOADS)/$(KTARBALL)"

# The tree is only ever built from a pristine tarball plus the patches in this
# repository, so a rebuild cannot silently inherit local edits.
tree: $(DOWNLOADS)/$(KTARBALL)
	@if [ -d $(TREE) ]; then echo "$(TREE) exists; run 'make clean' first"; exit 1; fi
	@mkdir -p build
	tar -C build -xf $(DOWNLOADS)/$(KTARBALL)
	@for p in $(PATCHES); do \
	    echo "applying $$p"; \
	    patch -d $(TREE) -p1 --forward --no-backup-if-mismatch < $$p || exit 1; \
	done
	@echo "tree ready: $(TREE)"

config: $(TREE)/.config
$(TREE)/.config: $(DEFCONFIG)
	@test -d $(TREE) || { echo "run 'make tree' first"; exit 1; }
	cp $(DEFCONFIG) $(TREE)/.config
	$(MAKEKERNEL) olddefconfig

kernel: config
	$(MAKEKERNEL) Image dtbs

dtbs: config
	$(MAKEKERNEL) dtbs

artifacts: kernel
	@mkdir -p $(OUT)
	cp $(TREE)/arch/arm64/boot/Image $(OUT)/
	@for d in $(DTBS); do cp $(TREE)/$(DTB_DIR)/$$d $(OUT)/ 2>/dev/null || echo "missing $$d"; done
	@cd $(OUT) && sha256sum Image *.dtb > SHA256SUMS
	@echo; echo "artifacts in $(OUT):"; ls -la $(OUT)

flash-help:
	@echo "Flashing writes to a physical card and is not automated from here."
	@echo "Read docs/flash.md. Back up the stock partitions first, and keep them."

clean:
	rm -rf build $(OUT)

distclean: clean
	rm -rf $(DOWNLOADS)
