# Flashing

> **The stock partition backup is a precondition, not a suggestion.** This
> repository does not contain the stock image and never will (see `NOTICE.md`).
> Your own backup is the only way back. Do not continue until it is taken and
> verified.
>
> The current build's initialisation is unresolved, so you should expect to need
> that backup.

## What gets written

One partition: **p8**, 64 MiB, at offset 61,890,101,248 on a 62,516,101,120-byte
card. Nothing else is touched. p4 (the stock 64 MiB partition at
47,332,720,640) is checked before and after and must be byte-identical.

## The guards

`tools/flash-rg40xxv-p8-wsl.sh` refuses to write unless all of these hold:

1. The disk is exactly the expected size.
2. The GPT disk GUID matches the one you configured.
3. All eight partition offsets and sizes match.
4. The p8 currently on the card is on an allowlist of known-good images.
5. A full pre-write backup exists.
6. The candidate image's SHA-256 matches the digest you passed in.

After writing it reads the whole 64 MiB back and compares, re-checks p4, and
restores the read-only guard.

**The card GUID ships as `PUT-YOUR-OWN-CARD-GPT-GUID-HERE`.** The scripts will
not run until you replace it. Find yours with:

```bash
sudo sfdisk --dump /dev/sdX | grep -i uuid    # Linux
```

Then set it in `tools/flash-rg40xxv-p8-wsl.sh` and the other scripts that carry
it. Do not skip this by deleting the check — that check is the only thing
standing between a typo and someone else's disk.

## Procedure

```bash
# 0. Identify the card. Read-only, writes nothing.
sudo tools/identify-rg40xxv-p8-wsl.sh /dev/sdX

# 1. Back up every partition and verify it. Keep this somewhere else.
sudo tools/collect_rg40xxv_readonly.sh /dev/sdX /path/to/backup

# 2. Audit the card read-only before touching it.
sudo tools/arm-rg40xxv-tf1-readonly.sh /dev/sdX

# 3. Write p8 only.
sudo tools/flash-rg40xxv-p8-wsl.sh \
     out/candidate-p8.img \
     <expected-sha256-of-that-image> \
     /path/to/backup \
     /dev/sdX
```

The script prints `P8_FLASH result=PASS` or a `result=FAIL reason=...` line. A
failure before the write means nothing was written.

## On WSL

The card may not be visible to Windows, in which case `wsl --unmount` cannot
find the PhysicalDrive and you write from inside WSL. The `.ps1` scripts in
`tools/` are the Windows-side equivalents and carry the same guards.

## Recovery

Restore your backup. That is the whole plan, and it is why step 1 is not
optional. If you did not take one, the stock image has to come from Anbernic,
not from here.

## Booting, rebooting, and getting back

The device boots what is in p8. There is no boot menu to pick from and no
fallback slot: whatever you wrote is what runs.

**First boot after flashing.** Eject the card properly, reseat it, and power the
device off completely before powering it on. A warm reboot out of a half-running
system exercises a different path, and this port's initialisation is not fixed
yet — cold boot is the honest test.

**Rebooting later.** Once a build gets far enough to give you a shell (serial,
SSH or ADB), `reboot` and `poweroff` work normally. If init has not come up
there is nothing listening, and the only option is holding the power button or
pulling the card.

**Watch the first frames.** A dark panel is usually the wrong dtb for your
hardware revision, not a dead device. Try the other RG40XX V device tree — the
plain one and the `-v2-panel` one target different panel revisions.

**Getting back to stock.** Write your p8 backup to the same offset the flasher
used. That is the entire recovery procedure, and it works because only p8 was
ever touched. If you skipped the backup, the stock image has to come from
Anbernic; it is not in this repository.
