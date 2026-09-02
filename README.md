# RG40XX V — mainline Linux kernel 7.2

**Anbernic RG40XX V (Allwinner H700) on mainline Linux kernel 7.2 instead of the
vendor BSP** — kernel patches, a launcher, system services, and a build and flash
procedure you can repeat.

It started from two things I wanted the device to do — game streaming that
actually works, and RPG Maker titles that run — and the kernel and display work
is what turned out to be in the way.

![The launcher running on the device: a game library view rendered by the mainline
display stack](docs/images/rg40xxv-shell.jpg)

> ## ⚠ Status: cold-boot panel initialisation is the open problem
>
> **Where it stands.** With the stock panel data imported and a clean reflash,
> the device comes up as far as the **boot menu**. That path works because
> something has already lit the panel and the driver adopts the running state.
>
> **What is not solved.** After a full power-off, nothing has lit the panel and
> it must be brought up from cold. That initialisation is the problem I am
> working on now, and until it is done this is not daily-driver firmware.
>
> The two are the same wall from different sides: the adoption path avoids
> replaying an init sequence Anbernic never documented, and a cold start is
> exactly the case where there is nothing to adopt.
>
> **Back up the stock partitions before you touch anything, and keep that
> backup.** Step 1 of [`docs/flash.md`](docs/flash.md) is the backup, and it is a
> precondition rather than a suggestion. Without it there is no way back: the
> stock image is not in this repository and never will be (see
> [`NOTICE.md`](NOTICE.md)), so your own backup is the only recovery source — and
> it is also where the panel data in [`docs/firmware.md`](docs/firmware.md) has
> to come from.
>
> The flashing script writes only the 64 MiB p8 boot partition and leaves the
> rest alone — but that is a guard rail, not a guarantee. Take the backup.

---

## What this solves

Linux 7.2 already boots on the H700 and already carries device trees for several
Anbernic handhelds. What it did not do was light this panel and keep it lit. Most
of the work here is display, and three problems were worth the trouble.

### A black screen from mixing two RDMA architectures

The symptom was a lit backlight with nothing on it. The cause was not the
userland writing panel registers behind the driver's back, which is where the
investigation started; it was inside the kernel.

The DE33 mixer had been moved to a **two-phase RDMA enable** flow, but its
initialisation never registered the blender and the formatter for deferred
enable. The first atomic commit therefore brought up a pipeline that was missing
two stages, and the panel showed black.

Only two combinations are coherent:

| Core | Deferred registration | Result |
| --- | --- | --- |
| single-phase RDMA | absent | works |
| two-phase RDMA | **present** | works |
| two-phase RDMA | absent | **black screen** |

The fix is the two `sun8i_rdma_defer_enable()` calls that make the third row
impossible. The interesting part is not the diff, it is that a display bug can
look exactly like an application misbehaving, and the way to tell them apart was
to establish which side of the boundary ever touches DE33, TCON or panel timing
registers at all. It turned out the release partition only ever issues
`FBIOBLANK`, `FBIO_WAITFORVSYNC` and VT ioctls — so the kernel was the only
suspect left.

### Adopting a panel instead of re-initialising it

These panels come up already scanning: the stock boot chain lights them before
Linux starts, and the init sequence it used is not documented anywhere. Replaying
a guessed sequence is how you get a dark or flickering screen.

So the panel driver detects that the panel is already lit and scanning and
**adopts** that state — it references the clocks it must hold, reads back the
timing, and does not re-program the mode or replay a reset. The kernel log says
so explicitly:

```
RG40XXV: panel adopted from firmware scanout (lit-at-probe=1 scanning-now=1):
         no reset/init replay, PI_DATA=...
TCON0 timing adopted from firmware (stock sw_enable): mode not re-programmed
```

This is a general technique for hardware whose panel init is a vendor secret:
inherit the working state rather than compete with it.

### Bringing the display down in a defined order

The other half of a display that survives a reboot is shutdown. `sun4i_drv` and
the mixer now quiesce the pipeline deliberately instead of leaving whatever state
happened to be current for the next stage to inherit.

### And the rest

- **Panfrost GPU faults.** Fixes across device, GPU, MMU and job handling for
  this SoC. This is the open Mali driver, not a vendor blob.
- **A joypad that needed a removed API.** The single-ADC joypad driver from
  ROCKNIX depends on `input-polldev`, which mainline had deleted. It is restored
  here — 362 lines — rather than rewriting a working driver.
- **A backlight controller with no driver.** `pwm-sun8i.c`, 1,542 new lines.
- Battery reporting, video decode, USB gadget, audio, watchdog, Bluetooth,
  suspend, and device trees for the whole Anbernic H700 family.

Details and the per-subsystem breakdown: [`docs/porting.md`](docs/porting.md).

## Status

Kernel and display are the part that works. The userland is further behind, and
two features are integrated only far enough to be honest about being unfinished.

| | State |
| --- | --- |
| Boot to a lit panel | works — see the display section above |
| Panfrost GPU | works |
| Joypad, buttons, battery, audio, watchdog | works |
| Launcher (`components/rg40xxv-shell`) | works — the screenshot is it running |
| Device services (power, network, Bluetooth, save-guard) | works |
| Screen off / on (`ui-hardwarectl`) | works — saves and kills the backlight only. It does not power down `fb0`, close the TCON, reset the panel or unprepare the DRM panel, because re-initialising this panel is the hard part. Verified on the device for one off/on round, not for suspend-to-RAM. |
| **YouTube (`components/youtube`)** | **host and QEMU gates pass; every device gate is `PENDING`.** A controller-first native client: SDL texture UI, libmpv/FFmpeg for media, and `yt-dlp` confined to an owner-private resolver service that is neither the UI nor the decoder. One exact-binary run on the device logged playback, an advancing ALSA pointer and a non-uniform frame, but under `evidence_scope=COMPONENT_GATE` on a non-target p8 — which is not user visual or audio acceptance, and the tile says `VERIFY`, not READY. |
| Boot menu, after a clean reflash with stock panel data | reached |
| **Cold-boot panel initialisation** | **unresolved — being worked on. This is the blocker; see the warning above.** |
| **Game streaming** | **not supported.** Only the settings backend exists: `components/netstream` stores Sunshine/Moonlight host profiles. It does not connect, does not invoke Moonlight, and no client is wired up. |
| **RPG Maker MV/MZ** | **not supported.** The launcher shows an `RPG · MV/MZ PENDING` tab. The runtime path (NW.js on aarch64) and a translation data path are partially integrated; neither is finished. The translation path is original work. |

Neither of the last two should be read as "nearly done". They are integration
points with their dependencies identified, and that is all.

## Rebuild

The tree is always built from a pristine kernel tarball plus the patches in this
repository, so a build cannot silently inherit someone's local edits.

```bash
make deps        # check the toolchain
make all         # fetch -> patch -> configure -> build -> collect into out/
```

Verified on 2026-08-30 from a clean `linux-7.2.tar.xz`: 120 files patched with no
rejects, 2,730 compile units, **0 errors**, and both RG40XX V device trees.

Two things the shipped configuration deliberately does **not** do, because the
first build here did them by accident and would not have worked on anyone else's
machine:

- It embeds **no initramfs**. The development config pointed at a rootfs
  directory outside the repository, so that build silently pulled in 5 MB of
  userland that a clone would not have.
- It embeds **no firmware**. The development config built Realtek Wi-Fi and
  Bluetooth blobs, plus a panel blob extracted from the stock image, straight
  into the kernel. Both settings are cleared; see
  [`docs/firmware.md`](docs/firmware.md) for what you have to supply and where
  each piece comes from.

So a clone builds a kernel that loads its firmware at runtime and boots whatever
root you give it. Building is not booting — see the status warning above.

## Flash

```bash
tools/deploy.sh /dev/sdX /path/to/backup out/candidate-p8.img
```

A guided sequence, not an automation: it identifies the card read-only, refuses
to continue without a verified backup, audits before touching anything, writes
**only** the 64 MiB p8 partition, reads the whole range back and compares, then
stops and asks you to cold-boot the device by hand.

The scripts ship with a placeholder where the card's GPT GUID goes and **refuse
to run until you put your own in**. That guard is the only thing between a typo
and someone else's disk; do not delete it.

Full procedure, including how to get back to stock:
[`docs/flash.md`](docs/flash.md).

## Layout

| Path | |
| --- | --- |
| `patches/` | The kernel port: 120 files, +10,326 / −1,038 against pristine 7.2. GPL-2.0. |
| `configs/` | The production kernel configuration, 1,833 options. |
| `tools/` | Identify, audit, flash and deploy scripts. |
| `components/rg40xxv-shell` | The launcher in the screenshot. MIT. |
| `components/device-control` | Power, network, save-guard, CPU policy, volume, USB debug and screen control services. |
| `components/bluetooth-runtime` | The Bluetooth control helper, model and systemd payload. It used to be a shell script inside `device-control`; it is now a C program with its own tests. |
| `components/youtube` | The native YouTube client. Needs `vendor/fetch-yt-dlp.sh` run once before `build.sh`. |
| `components/netstream` | Streaming service. |
| `docs/` | Porting notes, upstream provenance, build and flash procedure. |

## Licence

Two licences, because there are two kinds of work here.

The kernel patches are **GPL-2.0-only**. That is not a preference — a
modification to the kernel is a derivative work of it, and its licence decides
the terms. Everything else is **MIT**. See [`LICENSE`](LICENSE).

Every external project this port draws on is listed with its revision and licence
in [`docs/upstream-sources.md`](docs/upstream-sources.md). One of them matters
more than the others: the joypad driver comes from **ROCKNIX** and stays GPL-2.0.

## What is not here

No stock firmware image, no card dumps, no games, no vendor blobs. The working
tree this was extracted from held 37 GB of exactly that; this repository is
20 MB and holds none of it.

[`NOTICE.md`](NOTICE.md) gives the reason for each exclusion **and where that
piece legitimately comes from** — because leaving something out without saying
where to find it just sends people looking in worse places. The short version:
the panel data comes from your own backup, the Realtek firmware from
linux-firmware, and the games from wherever you legitimately got them.

The screenshot above is deliberately the library view with placeholder covers
rather than one showing box art, for the same reason.
