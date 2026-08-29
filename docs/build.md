# Rebuilding

## What you need

Debian/Ubuntu:

```bash
sudo apt install gcc-aarch64-linux-gnu make bc bison flex libssl-dev \
                 libelf-dev xz-utils curl device-tree-compiler rsync
```

`make deps` prints this list and shows whether the cross compiler is found.

## One command

```bash
make all
```

That fetches `linux-7.2.tar.xz` from kernel.org, unpacks it into `build/`,
applies every patch in `patches/`, copies the production configuration, runs
`olddefconfig`, builds `Image` and the device trees, and collects the results
into `out/` with a `SHA256SUMS`.

Roughly 15 minutes on 16 cores; longer on fewer. Adjust with `make all JOBS=8`.

## Step by step

```bash
make fetch     # download the pristine tarball
make tree      # unpack and apply patches/
make config    # production .config, then olddefconfig
make dtbs      # device trees only — quick way to check the DTS work
make kernel    # Image + dtbs
make artifacts # copy Image and the RG40XX V dtbs into out/
```

`make tree` refuses to run if `build/linux-7.2` already exists. That is
deliberate: a rebuild always starts from the tarball plus the patches, so it
cannot quietly pick up edits someone left in the tree. `make clean` removes the
build tree and keeps the download.

## Why it is patches and not a forked tree

The port is 120 files on top of a released kernel. Kept as patches, it stays
reviewable, it rebases onto the next kernel with `patch --forward` telling you
exactly what conflicted, and it stays honest about being a derivative work of
the kernel rather than a new tree of its own.

If you want a git branch instead:

```bash
make tree
cd build/linux-7.2 && git init -q && git add -A && git commit -qm upstream+port
```

## Output

| File | |
| --- | --- |
| `out/Image` | The kernel. |
| `out/sun50i-h700-anbernic-rg40xx-v.dtb` | RG40XX V. |
| `out/sun50i-h700-anbernic-rg40xx-v-v2-panel.dtb` | The v2 panel revision. |
| `out/SHA256SUMS` | Digests — the flashing step wants an expected digest. |

Check which panel revision your unit has before picking a dtb. Flashing the
wrong one gives you a dark screen, not a brick, but you will be reflashing.

## Components

`components/` are separate userland programs with their own build scripts
(`build.sh` in each). They are not part of the kernel build and are not required
to boot.

## Verified

This patch set was rebuilt from a pristine `linux-7.2.tar.xz` on 2026-08-30:

| | |
| --- | --- |
| Patch application | 120 files, no rejects |
| Configuration | 1,833 options, `olddefconfig` clean |
| Build | 2,730 compile units, **0 errors** |
| `Image` | 21.6 MB with the shipped config; 23.4 MB before it was sanitised |
| `sun50i-h700-anbernic-rg40xx-v.dtb` | 37,080 bytes |
| `sun50i-h700-anbernic-rg40xx-v-v2-panel.dtb` | 37,088 bytes |

Toolchain: `aarch64-linux-gnu-gcc 11.4.0`, 16 jobs.

Building is not booting. The device side of this port is unresolved; see the
warning at the top of the README.

### What that first verification got wrong

It was run before the configuration was sanitised, and it was not as clean as it
looked. `CONFIG_INITRAMFS_SOURCE` pointed at a rootfs directory outside the
repository which happened to exist on the build machine, so a 5 MB initramfs was
embedded that a clone would never get. `CONFIG_EXTRA_FIRMWARE` likewise pulled
Realtek blobs and a panel blob out of a local directory.

Both are cleared in the shipped config, and the build was repeated with the
sanitised one — the configuration a clone actually gets. That build also
succeeds: 0 errors, a 21.6 MB `Image`, both RG40XX V device trees, and a 512-byte
empty initramfs where 5 MB of someone else's userland used to be. The 1.8 MB
difference between the two images is exactly what should not have been in there.

See [`firmware.md`](firmware.md) for what to supply at runtime instead.
