# What this repository does not contain, and why

This port was developed inside a 37 GB working tree that also held vendor
firmware, card images and a game library. None of that is here. The exclusions
are deliberate, and each has a reason.

## Excluded: vendor firmware images

The stock Anbernic firmware image, the mounted vendor rootfs and every card
backup taken during development are **not** included.

Those images are Anbernic's build of an Allwinner BSP. Whatever is GPL inside
them is obtainable from the parties who owe it; the rest — vendor userland,
configuration and branding — is theirs, and redistributing a whole image is not
something a third party may do. Nothing in this repository needs them: the port
targets mainline, and `docs/flash.md` writes only the boot partition.

## Excluded: games

The device's game library is not here and never will be. Those are other
people's copyrighted works, and the fact that a launcher can list them does not
make them redistributable. The UI in `components/rg40xxv-shell` scans whatever is
on the card at runtime; it ships with no content.

## Excluded: proprietary blobs and third-party runtimes

The working tree contained `linux-firmware`, a Cobalt runtime and an NW.js
build. All are obtainable from their own projects under their own terms, none is
this project's to relicense, and none is needed to build the kernel. They are
referenced in `docs/upstream-sources.md` instead.

## Excluded: build output and card dumps

Compiled kernels, packaged releases, partition dumps and capture logs are
rebuildable or device-specific. They would add tens of gigabytes and tell a
reader nothing the patches and the documentation do not.

## Third-party material that is included

Two files in `components/rg40xxv-shell/assets/` are not this project's work, and
both carry the licence their terms require:

| File | Origin | Licence |
| --- | --- | --- |
| `RG40XXV-Material-Icons.png` | A raster atlas of the glyphs this interface uses, generated from Google Material Symbols Rounded. The 15 MB source font is not bundled. | Apache-2.0 — full text in `assets/LICENSE.Apache-2.0`, notice in `assets/MATERIAL-SYMBOLS-NOTICE.md` |
| `RG40XXV-UI-Sans.otf` | Noto Sans CJK | SIL OFL 1.1 — full text and upstream detail in `assets/NOTO-CJK-COPYRIGHT` |

Inside the kernel patches, every file added by this port keeps the SPDX
identifier and the copyright of whoever wrote it. Three are worth naming:

- `drivers/input/joystick/rocknix-singleadc-joypad.c` — **ROCKNIX**, GPL-2.0-or-later,
  recorded with its upstream URL and the exact commit it came from.
- `drivers/input/input-polldev.c` — Dmitry Torokhov's polled-input implementation,
  restored from an earlier kernel, GPL-2.0-only.
- `arch/arm64/boot/dts/allwinner/sun50i-h700-anbernic-*.dts` — the Anbernic H700
  device trees build on Philippe Simons' upstream work, dual GPL-2.0/BSD-2-Clause
  as mainline device trees are.

`drivers/pwm/pwm-sun8i.c`, `drivers/gpu/drm/panel/panel-mipi.c` and the sun4i
RDMA files likewise carry their authors' notices.

## Where the excluded things legitimately come from

Leaving something out without saying where it comes from just sends people
looking in worse places. Each exclusion has a proper source.

| What | Where to get it |
| --- | --- |
| **Stock firmware image** | Anbernic publishes firmware for its devices through its own support channels. Better still, take your own backup before flashing — `docs/flash.md` step 1 — because that is the exact image your unit shipped with, and it is the only one guaranteed to match your hardware revision. |
| **Panel initialisation data** (`*.panel`) | Extract it from *your own* stock image or card backup. It is Anbernic's data and nobody else can hand it to you. This is one more reason the backup is step 1. See `docs/firmware.md`. |
| **Wi-Fi and Bluetooth firmware** | [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware) — `rtw88/rtw8821c_fw.bin`, `rtl_bt/rtl8821cs_fw.bin`, `rtl_bt/rtl8821cs_config.bin`, redistributable under Realtek's terms stated in that repository. |
| **NW.js runtime** (for RPG Maker MV/MZ) | [nwjs.io](https://nwjs.io) — official aarch64 builds, MIT for NW.js itself. |
| **Moonlight / Sunshine** | [moonlight-stream](https://github.com/moonlight-stream) and [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine), both GPL-3.0, from their own projects. |
| **The kernel itself** | [kernel.org](https://kernel.org). `make fetch` downloads it for you. |
| **Games** | Your own. Dumps of cartridges and discs you own, or titles you bought from a storefront that sells them. This project has no opinion beyond that, and no way to help you with it. |

## Sanitised: card identity

The flashing scripts guard on the GPT GUID of the card they may write to. That
GUID identified one specific card, so it is replaced with
`PUT-YOUR-OWN-CARD-GPT-GUID-HERE`. **The scripts will refuse to run until you
put your own card's GUID in.** That is the guard working as intended, not a bug.

A personal Windows path in one PowerShell helper was parameterised for the same
reason.

## Kept: a test that looks like a leak

`components/device-control/tests/test-vpn.sh` contains a synthetic key and the
string `super-secret-password`. It is a test that proves the VPN import path does
**not** write credentials into logs or nftables output. The values are fake and
exist so the assertion can fail loudly if that ever regresses.
