#!/usr/bin/env bash
set -euo pipefail

TEST_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd -- "$TEST_DIR/.." && pwd)
FIXTURE=$(mktemp -d /tmp/rg40xxv-device-control-test.saves.XXXXXX)
ROOTFS="$FIXTURE/rootfs"
trap 'rm -rf -- "$FIXTURE"' EXIT

mkdir -p \
    "$ROOTFS/.config/retroarch/saves" \
    "$ROOTFS/.config/retroarch/states" \
    "$ROOTFS/mnt/data/retroarch/saves" \
    "$ROOTFS/mnt/data/retroarch/states" \
	"$ROOTFS/mnt/data/rg40xxv/state/aarch64-retroarch/saves" \
	"$ROOTFS/mnt/data/rg40xxv/state/aarch64-retroarch/states" \
    "$ROOTFS/mnt/data/ppsspp/PSP/SAVEDATA/ULUS00001" \
    "$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SAVEDATA/ULUS00005" \
    "$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/PPSSPP_STATE" \
    "$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SCREENSHOT" \
    "$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/Cheats" \
    "$ROOTFS/mnt/data/drastic/backup" \
    "$ROOTFS/mnt/mmc/save_nds/backup" \
    "$ROOTFS/mnt/mmc/save_nds/savestates" \
    "$ROOTFS/mnt/sdcard/save_nds/backup" \
    "$ROOTFS/mnt/sdcard/save_nds/savestates" \
    "$ROOTFS/mnt/mmc/save_flycast/.local/share/flycast" \
    "$ROOTFS/mnt/sdcard/save_flycast/.local/share/flycast" \
    "$ROOTFS/mnt/mmc/.config/ppsspp/PSP/SAVEDATA/ULUS00003" \
    "$ROOTFS/mnt/mmc/.config/ppsspp/PSP/PPSSPP_STATE" \
    "$ROOTFS/mnt/mmc/.config/ppsspp/PSP/SCREENSHOT" \
    "$ROOTFS/mnt/mmc/.config/ppsspp/PSP/Cheats" \
    "$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/SAVEDATA/ULJM00004" \
    "$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/PPSSPP_STATE" \
    "$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/SCREENSHOT" \
    "$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/Cheats" \
    "$ROOTFS/mnt/sdcard/PSP/SAVEDATA/ULJM00002" \
    "$ROOTFS/mnt/sdcard/PSP/PPSSPP_STATE" \
    "$ROOTFS/mnt/vendor/deep/drastic-modify/res/backup" \
    "$ROOTFS/mnt/vendor/deep/drastic-modify/res/savestates" \
    "$ROOTFS/mnt/mmc/Roms/GBA/Saves" \
    "$ROOTFS/mnt/mmc/Roms/GBA/mGBA" \
    "$ROOTFS/mnt/mmc/Roms/PS/PCSX-ReARMed" \
    "$ROOTFS/mnt/mmc/Roms/N64/Mupen64Plus-Next" \
    "$ROOTFS/mnt/mmc/Roms/DOS/DOSBox-pure" \
    "$ROOTFS/mnt/mmc/Roms/RPG/EasyRPG Player" \
    "$ROOTFS/mnt/mmc/Roms/NDS/melonDS" \
    "$ROOTFS/mnt/mmc/Roms/PORTS/測試遊戲/savedata" \
    "$ROOTFS/mnt/mmc/Roms/OPENBOR/Chrono Killer/Saves" \
    "$ROOTFS/mnt/sdcard/Roms/GBC/mGBA" \
    "$ROOTFS/mnt/sdcard/Roms/OPENBOR/Final Fight/Saves" \
    "$ROOTFS/etc/NetworkManager/system-connections" \
    "$ROOTFS/root/.ssh"
printf 'STOCK-ROOT-RA-SAVE\n' >"$ROOTFS/.config/retroarch/saves/stock-root.srm"
printf 'STOCK-ROOT-RA-STATE\n' >"$ROOTFS/.config/retroarch/states/stock-root.state"
printf 'RA-SAVE\n' >"$ROOTFS/mnt/data/retroarch/saves/pokemon.srm"
printf 'RA-STATE\n' >"$ROOTFS/mnt/data/retroarch/states/pokemon.state"
printf 'MAINLINE-RA-SAVE\n' >"$ROOTFS/mnt/data/rg40xxv/state/aarch64-retroarch/saves/mainline.srm"
printf 'MAINLINE-RA-STATE\n' >"$ROOTFS/mnt/data/rg40xxv/state/aarch64-retroarch/states/mainline.state"
printf 'PSP-SAVE\n' >"$ROOTFS/mnt/data/ppsspp/PSP/SAVEDATA/ULUS00001/DATA.BIN"
printf 'MAINLINE-PPSSPP-SAVE\n' >"$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SAVEDATA/ULUS00005/DATA.BIN"
printf 'MAINLINE-PPSSPP-STATE\n' >"$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/PPSSPP_STATE/ULUS00005_1.00.ppst"
printf 'MAINLINE-PPSSPP-SCREENSHOT\n' >"$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SCREENSHOT/ULUS00005_00000.jpg"
printf 'MAINLINE-PPSSPP-CHEAT\n' >"$ROOTFS/mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/Cheats/ULUS00005.ini"
printf 'NDS-SAVE\n' >"$ROOTFS/mnt/data/drastic/backup/game.dsv"
printf 'MMC-NDS-BACKUP\n' >"$ROOTFS/mnt/mmc/save_nds/backup/mmc-game.dsv"
printf 'MMC-NDS-STATE\n' >"$ROOTFS/mnt/mmc/save_nds/savestates/mmc-game.dss"
printf 'TF2-NDS-BACKUP\n' >"$ROOTFS/mnt/sdcard/save_nds/backup/tf2-game.dsv"
printf 'TF2-NDS-STATE\n' >"$ROOTFS/mnt/sdcard/save_nds/savestates/tf2-game.dss"
printf 'MMC-FLYCAST-VMU\n' >"$ROOTFS/mnt/mmc/save_flycast/.local/share/flycast/vmu_save_A1.bin"
printf 'TF2-FLYCAST-VMU\n' >"$ROOTFS/mnt/sdcard/save_flycast/.local/share/flycast/vmu_save_A2.bin"
printf 'MMC-PPSSPP-SAVE\n' >"$ROOTFS/mnt/mmc/.config/ppsspp/PSP/SAVEDATA/ULUS00003/DATA.BIN"
printf 'MMC-PPSSPP-STATE\n' >"$ROOTFS/mnt/mmc/.config/ppsspp/PSP/PPSSPP_STATE/ULUS00003_1.00.ppst"
printf 'MMC-PPSSPP-SCREENSHOT\n' >"$ROOTFS/mnt/mmc/.config/ppsspp/PSP/SCREENSHOT/ULUS00003_00000.jpg"
printf 'MMC-PPSSPP-CHEAT\n' >"$ROOTFS/mnt/mmc/.config/ppsspp/PSP/Cheats/ULUS00003.ini"
printf 'TF2-PPSSPP-SAVE\n' >"$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/SAVEDATA/ULJM00004/DATA.BIN"
printf 'TF2-PPSSPP-STATE\n' >"$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/PPSSPP_STATE/ULJM00004_1.00.ppst"
printf 'TF2-PPSSPP-SCREENSHOT\n' >"$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/SCREENSHOT/ULJM00004_00000.jpg"
printf 'TF2-PPSSPP-CHEAT\n' >"$ROOTFS/mnt/sdcard/.config/ppsspp/PSP/Cheats/ULJM00004.ini"
printf 'TF2-PSP-SAVE\n' >"$ROOTFS/mnt/sdcard/PSP/SAVEDATA/ULJM00002/DATA.BIN"
printf 'TF2-PSP-STATE\n' >"$ROOTFS/mnt/sdcard/PSP/PPSSPP_STATE/ULJM00002_1.00.ppst"
printf 'DRASTIC-MODIFY-BACKUP\n' >"$ROOTFS/mnt/vendor/deep/drastic-modify/res/backup/modified-game.dsv"
printf 'DRASTIC-MODIFY-STATE\n' >"$ROOTFS/mnt/vendor/deep/drastic-modify/res/savestates/modified-game.dss"
printf 'GBA-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/GBA/Saves/game.sav"
printf 'CONTENT-MGBA-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/GBA/mGBA/pokemon.srm"
printf 'CONTENT-PCSX-STATE\n' >"$ROOTFS/mnt/mmc/Roms/PS/PCSX-ReARMed/final-fantasy.state"
printf 'CONTENT-MUPEN-EEP\n' >"$ROOTFS/mnt/mmc/Roms/N64/Mupen64Plus-Next/zelda.eep"
printf 'CONTENT-DOSBOX-OVERLAY\n' >"$ROOTFS/mnt/mmc/Roms/DOS/DOSBox-pure/doom.pure.zip"
printf 'CONTENT-EASYRPG-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/RPG/EasyRPG Player/Save01.lsd"
printf 'CONTENT-MELONDS-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/NDS/melonDS/pokemon.dsv"
printf 'PORT-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/PORTS/測試遊戲/savedata/profile.dat"
printf 'MMC-OPENBOR-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/OPENBOR/Chrono Killer/Saves/Chrono Killer.cfg"
printf 'TF2-CONTENT-STATE\n' >"$ROOTFS/mnt/sdcard/Roms/GBC/mGBA/crystal.state12"
printf 'TF2-OPENBOR-SAVE\n' >"$ROOTFS/mnt/sdcard/Roms/OPENBOR/Final Fight/Saves/Final Fight.cfg"
printf 'NOT-A-SAVE\n' >"$ROOTFS/mnt/mmc/Roms/GBA/mGBA/readme.txt"
ln -s "$ROOTFS/root/.ssh/id_ed25519" "$ROOTFS/mnt/mmc/Roms/GBA/mGBA/linked-secret.srm"
printf 'wifi-secret\n' >"$ROOTFS/etc/NetworkManager/system-connections/home.nmconnection"
printf 'ssh-secret\n' >"$ROOTFS/root/.ssh/id_ed25519"

ORIGINAL_HASHES=$(find "$ROOTFS/mnt" "$ROOTFS/.config" -type f -print0 | sort -z | xargs -0 sha256sum)
WIFI_HASH=$(sha256sum "$ROOTFS/etc/NetworkManager/system-connections/home.nmconnection")
SSH_HASH=$(sha256sum "$ROOTFS/root/.ssh/id_ed25519")

export DEVICE_CONTROL_TESTING=1
export DEVICE_CONTROL_TEST_ROOT="$FIXTURE"
CTL="$ROOT/save-guard/save-guard"

MANIFEST=$("$CTL" inventory)
[[ -f "$MANIFEST" ]]
[[ $(stat -c '%a' "$MANIFEST") == 600 ]]
for relative in \
    .config/retroarch/saves/stock-root.srm \
    .config/retroarch/states/stock-root.state \
    mnt/data/retroarch/saves/pokemon.srm \
	mnt/data/rg40xxv/state/aarch64-retroarch/saves/mainline.srm \
	mnt/data/rg40xxv/state/aarch64-retroarch/states/mainline.state \
    mnt/data/ppsspp/PSP/SAVEDATA/ULUS00001/DATA.BIN \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SAVEDATA/ULUS00005/DATA.BIN \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/PPSSPP_STATE/ULUS00005_1.00.ppst \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SCREENSHOT/ULUS00005_00000.jpg \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/Cheats/ULUS00005.ini \
    mnt/data/drastic/backup/game.dsv \
    mnt/mmc/save_nds/backup/mmc-game.dsv \
    mnt/mmc/save_nds/savestates/mmc-game.dss \
    mnt/sdcard/save_nds/backup/tf2-game.dsv \
    mnt/sdcard/save_nds/savestates/tf2-game.dss \
    mnt/mmc/save_flycast/.local/share/flycast/vmu_save_A1.bin \
    mnt/sdcard/save_flycast/.local/share/flycast/vmu_save_A2.bin \
    mnt/mmc/.config/ppsspp/PSP/SAVEDATA/ULUS00003/DATA.BIN \
    mnt/mmc/.config/ppsspp/PSP/PPSSPP_STATE/ULUS00003_1.00.ppst \
    mnt/mmc/.config/ppsspp/PSP/SCREENSHOT/ULUS00003_00000.jpg \
    mnt/mmc/.config/ppsspp/PSP/Cheats/ULUS00003.ini \
    mnt/sdcard/.config/ppsspp/PSP/SAVEDATA/ULJM00004/DATA.BIN \
    mnt/sdcard/.config/ppsspp/PSP/PPSSPP_STATE/ULJM00004_1.00.ppst \
    mnt/sdcard/.config/ppsspp/PSP/SCREENSHOT/ULJM00004_00000.jpg \
    mnt/sdcard/.config/ppsspp/PSP/Cheats/ULJM00004.ini \
    mnt/sdcard/PSP/SAVEDATA/ULJM00002/DATA.BIN \
    mnt/sdcard/PSP/PPSSPP_STATE/ULJM00002_1.00.ppst \
    mnt/vendor/deep/drastic-modify/res/backup/modified-game.dsv \
    mnt/vendor/deep/drastic-modify/res/savestates/modified-game.dss \
    mnt/mmc/Roms/GBA/Saves/game.sav \
    mnt/mmc/Roms/GBA/mGBA/pokemon.srm \
    mnt/mmc/Roms/PS/PCSX-ReARMed/final-fantasy.state \
    mnt/mmc/Roms/N64/Mupen64Plus-Next/zelda.eep \
    mnt/mmc/Roms/DOS/DOSBox-pure/doom.pure.zip \
    'mnt/mmc/Roms/RPG/EasyRPG Player/Save01.lsd' \
    mnt/mmc/Roms/NDS/melonDS/pokemon.dsv \
    mnt/sdcard/Roms/GBC/mGBA/crystal.state12 \
    'mnt/mmc/Roms/PORTS/測試遊戲/savedata/profile.dat' \
    'mnt/mmc/Roms/OPENBOR/Chrono Killer/Saves/Chrono Killer.cfg' \
    'mnt/sdcard/Roms/OPENBOR/Final Fight/Saves/Final Fight.cfg'; do
    encoded=$(printf '%s' "$relative" | base64 -w0)
    grep -q "${encoded}$" "$MANIFEST"
done
for relative in \
    etc/NetworkManager/system-connections/home.nmconnection \
    root/.ssh/id_ed25519 \
    mnt/mmc/Roms/GBA/mGBA/linked-secret.srm \
    mnt/mmc/Roms/GBA/mGBA/readme.txt; do
    encoded=$(printf '%s' "$relative" | base64 -w0)
    if grep -q "${encoded}$" "$MANIFEST"; then
        printf 'FAIL: inventory 掃到非白名單檔案：%s\n' "$relative" >&2
        exit 1
    fi
done

BACKUP=$("$CTL" backup-before-core-switch --from mgba-old --to mgba-aarch64)
[[ -d "$BACKUP" ]]
[[ -f "$BACKUP/manifest.tsv" && -f "$BACKUP/saves.tar.gz" ]]
grep -q '^from_core=mgba-old$' "$BACKUP/core-switch.txt"
grep -q '^to_core=mgba-aarch64$' "$BACKUP/core-switch.txt"
ARCHIVE_LIST="$FIXTURE/archive.list"
tar -tzf "$BACKUP/saves.tar.gz" >"$ARCHIVE_LIST"
grep -Fqx 'mnt/data/retroarch/saves/pokemon.srm' "$ARCHIVE_LIST"
grep -Fqx 'mnt/data/rg40xxv/state/aarch64-retroarch/saves/mainline.srm' "$ARCHIVE_LIST"
grep -Fqx 'mnt/data/rg40xxv/state/aarch64-retroarch/states/mainline.state' "$ARCHIVE_LIST"
grep -Fqx 'mnt/mmc/Roms/PORTS/測試遊戲/savedata/profile.dat' "$ARCHIVE_LIST"
for relative in \
    .config/retroarch/saves/stock-root.srm \
    .config/retroarch/states/stock-root.state \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SAVEDATA/ULUS00005/DATA.BIN \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/PPSSPP_STATE/ULUS00005_1.00.ppst \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/SCREENSHOT/ULUS00005_00000.jpg \
    mnt/data/rg40xxv/state/home/.config/ppsspp/PSP/Cheats/ULUS00005.ini \
    mnt/mmc/save_nds/backup/mmc-game.dsv \
    mnt/mmc/save_nds/savestates/mmc-game.dss \
    mnt/sdcard/save_nds/backup/tf2-game.dsv \
    mnt/sdcard/save_nds/savestates/tf2-game.dss \
    mnt/mmc/save_flycast/.local/share/flycast/vmu_save_A1.bin \
    mnt/sdcard/save_flycast/.local/share/flycast/vmu_save_A2.bin \
    mnt/mmc/.config/ppsspp/PSP/SAVEDATA/ULUS00003/DATA.BIN \
    mnt/mmc/.config/ppsspp/PSP/PPSSPP_STATE/ULUS00003_1.00.ppst \
    mnt/mmc/.config/ppsspp/PSP/SCREENSHOT/ULUS00003_00000.jpg \
    mnt/mmc/.config/ppsspp/PSP/Cheats/ULUS00003.ini \
    mnt/sdcard/.config/ppsspp/PSP/SAVEDATA/ULJM00004/DATA.BIN \
    mnt/sdcard/.config/ppsspp/PSP/PPSSPP_STATE/ULJM00004_1.00.ppst \
    mnt/sdcard/.config/ppsspp/PSP/SCREENSHOT/ULJM00004_00000.jpg \
    mnt/sdcard/.config/ppsspp/PSP/Cheats/ULJM00004.ini \
    mnt/sdcard/PSP/SAVEDATA/ULJM00002/DATA.BIN \
    mnt/sdcard/PSP/PPSSPP_STATE/ULJM00002_1.00.ppst \
    mnt/vendor/deep/drastic-modify/res/backup/modified-game.dsv \
    mnt/vendor/deep/drastic-modify/res/savestates/modified-game.dss \
    mnt/mmc/Roms/GBA/mGBA/pokemon.srm \
    mnt/mmc/Roms/PS/PCSX-ReARMed/final-fantasy.state \
    mnt/mmc/Roms/N64/Mupen64Plus-Next/zelda.eep \
    mnt/mmc/Roms/DOS/DOSBox-pure/doom.pure.zip \
    'mnt/mmc/Roms/RPG/EasyRPG Player/Save01.lsd' \
    mnt/mmc/Roms/NDS/melonDS/pokemon.dsv \
    mnt/sdcard/Roms/GBC/mGBA/crystal.state12 \
    'mnt/mmc/Roms/OPENBOR/Chrono Killer/Saves/Chrono Killer.cfg' \
    'mnt/sdcard/Roms/OPENBOR/Final Fight/Saves/Final Fight.cfg'; do
    grep -Fqx "$relative" "$ARCHIVE_LIST"
done
if grep -Eq 'NetworkManager|\.ssh|linked-secret|readme\.txt' "$ARCHIVE_LIST"; then
    printf 'FAIL: 備份包含網路或 SSH 資料\n' >&2
    exit 1
fi

[[ $(find "$ROOTFS/mnt" "$ROOTFS/.config" -type f -print0 | sort -z | xargs -0 sha256sum) == "$ORIGINAL_HASHES" ]]
[[ $(sha256sum "$ROOTFS/etc/NetworkManager/system-connections/home.nmconnection") == "$WIFI_HASH" ]]
[[ $(sha256sum "$ROOTFS/root/.ssh/id_ed25519") == "$SSH_HASH" ]]

if "$CTL" backup-before-core-switch --from '../../bad' --to good >/dev/null 2>&1; then
    printf 'FAIL: 接受不安全核心名稱\n' >&2
    exit 1
fi
if find "$ROOTFS/var/lib/rg40xxv/save-guard" -name '.backup.*' -o -name '.roots.*' | grep -q .; then
    printf 'FAIL: 成功操作後留下暫存檔\n' >&2
    exit 1
fi

printf 'PASS save-guard：原廠 RA content-dir sidecar／NDS／DraStic Modify／Flycast／OpenBOR／PPSSPP／ROM／PORTS、原子 manifest、不追 symlink、原存檔不變\n'
