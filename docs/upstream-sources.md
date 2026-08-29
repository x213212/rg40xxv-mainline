# Upstream sources

Every external project this port draws on, with the revision that was actually
consulted. None of them is vendored here; clone them yourself if you want to
read along.

## The kernel

| | |
| --- | --- |
| Project | Linux 7.2 |
| Source | <https://kernel.org> — `linux-7.2.tar.xz` |
| Licence | GPL-2.0-only (`LICENSE.GPL-2.0`) |
| Role | The base. `patches/` applies on top of a pristine tree. |

The Allwinner H700 and several Anbernic devices are already supported upstream;
`sun50i-h700-anbernic-rg35xx-h.dts` and friends ship in 7.2. This port extends
that support rather than starting from a vendor BSP, which is why the patch set
is readable and why the display work is worth upstreaming.

## Projects consulted during the port

| Project | Revision | Source | Licence | What it was used for |
| --- | --- | --- | --- | --- |
| ROCKNIX distribution | `ea48bd37fa` (2026-08-23) | <https://github.com/ROCKNIX/distribution> | GPL-2.0 | The single-ADC joypad driver. `drivers/input/joystick/rocknix-singleadc-joypad.c` in the patch set derives from ROCKNIX's driver and stays GPL-2.0. |
| U-Boot | `127a42c7` (2026-01-05) | <https://github.com/u-boot/u-boot> | GPL-2.0+ | Mainline boot loader reference. |
| U-Boot (OrangePi, sun50iw9) | `2730962` (2026-03-04) | <https://github.com/orangepi-xunlong/u-boot-orangepi> | GPL-2.0+ | The vendor boot flow for this SoC family: partition layout and the boot0 handoff. |
| allwinner-bare-metal | `3ca22ad` (2024-03-04) | <https://github.com/uli/allwinner-bare-metal> | see repository | Display engine and TCON register behaviour, verified without a kernel in the way. |
| MinUI | `dbf8943` (2025-11-27) | <https://github.com/shauninman/MinUI> | see repository | Handheld launcher prior art. |
| muOS frontend | `b43ed9f` (2026-08-23) | <https://github.com/MustardOS/frontend> | see repository | Handheld launcher prior art. |
| EmulationStation-DE | `563fd73` (2026-08-22) | <https://gitlab.com/es-de/emulationstation-de> | see repository | Frontend behaviour reference. |
| GreenOvercast | `1a95bf6` (2026-08-11) | <https://github.com/Producdevity/GreenOvercast> | see repository | Reference. |
| linux-firmware | `8c7fac6` (2026-08-21) | <https://gitlab.com/kernel-firmware/linux-firmware> | per-file, see repository | Wi-Fi firmware for the running system. Not redistributed here. |
| Cobalt (Evergreen arm64) | 7.1.2 | Google | proprietary | Evaluated as a media runtime. Not redistributed, not required. |

## Projects the unfinished features depend on

Streaming and RPG Maker MV/MZ are **not supported** (see the status table in the
README). They are listed here because the integration work that does exist is
built against these projects, and they deserve the credit either way.

| Project | Source | Licence | Role here |
| --- | --- | --- | --- |
| Moonlight | <https://github.com/moonlight-stream> | GPL-3.0 | The streaming client this device would use. `components/netstream` stores its host configuration; it does not invoke it, and no client is integrated. |
| Sunshine | <https://github.com/LizardByte/Sunshine> | GPL-3.0 | The streaming host Moonlight connects to. Same status: configuration only. |
| NW.js | <https://nwjs.io> — <https://github.com/nwjs/nw.js> | MIT (Chromium and its dependencies carry their own) | RPG Maker MV/MZ games are NW.js applications. An aarch64 NW.js runtime is what would run them. Not redistributed here; not finished. |
| GreenOvercast | <https://github.com/Producdevity/GreenOvercast> `1a95bf6` | see repository | Consulted while looking at streaming on handhelds. |
| RMUT translation path | original work | this project's | The PC-side exporter and the device-side adapter were both written for this project. Not a fork or a port. Reads translation packages placed next to each game. |

## Attribution that matters

The joypad driver is the one place where another project's code is carried
directly. It comes from ROCKNIX, it is GPL-2.0, and it stays GPL-2.0 here. If
this port is ever submitted upstream, that provenance belongs in the commit
message.
