# What was ported

Linux 7.2 already supports the Allwinner H700 and several Anbernic handhelds.
This port adds the RG40XX V and, mostly, makes the display engine behave.

`patches/0001-rg40xxv-mainline-7.2.patch` — 120 files, +10,326 / −1,038 lines
against a pristine 7.2 tree.

| Area | Files | Added |
| --- | ---: | ---: |
| `drivers/gpu/drm` (sun4i, panfrost, panel) | 48 | 4,842 |
| `arch/arm64/boot/dts/allwinner` | 30 | 1,333 |
| `drivers/pwm/pwm-sun8i.c` | 1 | 1,542 |
| `drivers/input/joystick` | 4 | 1,497 |
| `drivers/input/input-polldev.c` | 1 | 362 |
| `Documentation/devicetree/bindings` | 9 | 232 |
| everything else | 27 | ~520 |

## Display — the bulk of the work

The H700's display path is a DE2/DE33-era mixer feeding a TCON, and mainline's
`sun4i` driver did not drive this particular combination correctly. The patch
touches the mixer, TCON, TCON-top, RDMA, the UI and VI layers and their scalers,
the CSC block, the HDMI PHY, the DE2 clock driver and the IOMMU.

The iteration history is legible in the working tree's backup files, and the
names say what each attempt was for: `before-h700-tcon`, `before-forced-pio`,
`before-forced-de-reset`, `before-symmetric-prepare`,
`before-forced-panel-recycle`, `before-shutdown-quiesce`, `before-gpu-fault-fix`.

Two problems are worth calling out because they shape the patch:

- **Shutdown quiesce.** The display had to be brought down in a defined order
  rather than left for the next stage to inherit. `sun4i_drv.c` and the mixer
  carry that sequencing.
- **Panfrost GPU faults.** The Mali driver — the open one, not a blob — needed
  fixes around device/GPU/MMU/job handling for this SoC.

A `panel-mipi` driver and binding were added for the panel, and the v2 panel
revisions get their own device trees.

## Device trees

The whole Anbernic H700 family is covered, not just the target: RG28XX, RG34XX
(and SP), RG35XX (2024, H, Plus, Pro, SP), RG40XX H and V, RG CubeXX, RG SP —
each with its v2/rev6 panel variant where one exists. Several shared `.dtsi`
files and the H616/H618 boards were touched by the same display changes.

## Input

The single-ADC joypad driver comes from ROCKNIX (GPL-2.0, see
`docs/upstream-sources.md`). Supporting it meant restoring `input-polldev`,
which mainline had removed — that is what the 362 added lines are.

`adc-keys` was extended for the remaining buttons.

## PWM

`pwm-sun8i.c` is new: 1,542 lines for the backlight controller this SoC uses.

## Everything else

Battery reporting (`axp20x_battery`), the video decoder (`cedrus`), USB
(`musb_gadget`, `phy-sun4i-usb`), audio (`sun4i-codec`), the watchdog, Bluetooth
(`btrtl`, `btusb`), suspend (`kernel/power/main.c`) and the initramfs.

## Status

Ask the working tree, not this file. The port was developed against a physical
card and the honest position at the time of extraction was that a candidate
passes host-side gates and then has to be proven on the device. Nothing here
should be called golden until it has booted on your hardware.
