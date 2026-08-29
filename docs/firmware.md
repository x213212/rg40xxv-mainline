# Firmware you have to supply

The shipped configuration builds a kernel that loads firmware from the
filesystem at runtime. It does **not** embed any, and this repository ships
none. Three things are needed, from three different places, and only one of them
can be downloaded.

## Why the config was changed

The production build embedded four files directly into the kernel image:

```
CONFIG_EXTRA_FIRMWARE="panels/anbernic,rg40xx-v2-stock-panel.panel
                       rtw88/rtw8821c_fw.bin
                       rtl_bt/rtl8821cs_fw.bin
                       rtl_bt/rtl8821cs_config.bin"
CONFIG_EXTRA_FIRMWARE_DIR="/some/local/path"
```

Both settings are cleared in `configs/rg40xxv_production_defconfig`. Embedding
them would put a Realtek redistributable and a blob extracted from Anbernic's
firmware inside every `Image` built from this repository, and neither is this
project's to hand out. Clearing them also makes the build work on a machine that
is not the one it was developed on.

## 1. Wi-Fi — `rtw88/rtw8821c_fw.bin`

From [linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware), under
Realtek's redistribution terms in that repository. Install it as
`/lib/firmware/rtw88/rtw8821c_fw.bin` on the device.

## 2. Bluetooth — `rtl_bt/rtl8821cs_fw.bin`, `rtl_bt/rtl8821cs_config.bin`

Same source, same terms, install under `/lib/firmware/rtl_bt/`.

## 3. Panel — `panels/anbernic,rg40xx-v2-stock-panel.panel`

**This one cannot be downloaded.** It is the panel initialisation data that
Anbernic's own firmware uses, and it is theirs. You extract it from the stock
image on *your* device — which is one more reason step 1 of `docs/flash.md` is to
take and keep a full backup before changing anything.

The driver reads it as a small container identified by the ASCII magic
`PANEL-FIRMWARE`. Note that the display path is written to **adopt** a panel the
boot chain has already lit (see the README), so this data is what lets a cold
path re-establish the same state rather than guess at it.

## Rebuilding with them embedded

If you would rather build a self-contained image for your own use, put the files
in a directory and point the config back at it:

```
CONFIG_EXTRA_FIRMWARE_DIR="/path/to/your/firmware"
CONFIG_EXTRA_FIRMWARE="panels/anbernic,rg40xx-v2-stock-panel.panel rtw88/rtw8821c_fw.bin rtl_bt/rtl8821cs_fw.bin rtl_bt/rtl8821cs_config.bin"
```

That image is then yours to keep, not to publish.
