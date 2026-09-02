#!/usr/bin/env bash
set -euo pipefail

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
workspace=$(CDPATH= cd -- "$project/../../../.." && pwd -P)
rootfs=${RG40XXV_STOCK_ROOTFS:-$workspace/firmware/mnt/rootfs}
firmware_source=${RG40XXV_FIRMWARE_SOURCE:-$workspace/build-inputs/rg40xxv-test1-firmware}
linux_firmware_repo=${RG40XXV_LINUX_FIRMWARE_REPO:-$workspace/references/linux-firmware}
build=$project/build
stage=$build/release-root
cc=${AARCH64_CC:-aarch64-linux-gnu-gcc-12}
source_date_epoch=${SOURCE_DATE_EPOCH:-1787702400}
archive=$build/rg40xxv-bluetooth-runtime-candidate-v1.tar.xz

export LC_ALL=C
export SOURCE_DATE_EPOCH=$source_date_epoch

die()
{
	printf 'BLUETOOTH_RUNTIME_BUILD result=FAIL reason=%s\n' "$1" >&2
	exit 1
}

require_file()
{
	[[ -f $1 && ! -L $1 ]] || die "missing-or-symlinked-file:$1"
}

check_sha()
{
	local expected=$1
	local path=$2
	local actual

	require_file "$path"
	actual=$(sha256sum "$path" | awk '{print $1}')
	[[ $actual == "$expected" ]] || die "dependency-hash:$path"
}

case $source_date_epoch in
	''|*[!0-9]*) die source-date-epoch ;;
esac
for tool in "$cc" awk bash cmp file find git install readelf realpath sha256sum \
	sort tar touch xargs xz; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done

rootfs=$(realpath -e -- "$rootfs")
firmware_source=$(realpath -e -- "$firmware_source")
linux_firmware_repo=$(realpath -e -- "$linux_firmware_repo")
[[ -d $rootfs && ! -L $rootfs ]] || die unsafe-stock-rootfs
[[ -d $firmware_source && ! -L $firmware_source ]] || die unsafe-firmware-source
[[ -d $linux_firmware_repo/.git && ! -L $linux_firmware_repo ]] || \
	die unsafe-linux-firmware-repository
[[ $(git -C "$linux_firmware_repo" rev-parse HEAD) == \
	8c7fac62c0d1c3b8915f596effc1ef6e95fd6b5f ]] || \
	die linux-firmware-commit-drift
case "$build/" in "$project/build/") ;; *) die unsafe-build-root ;; esac
[[ ! -L $build ]] || die symlinked-build-root

check_sha ddeb2198f1ba0a87d165dcb0111f904808647a5f01834f41f514267d2650091c \
	"$rootfs/usr/libexec/bluetooth/bluetoothd"
check_sha cde6a28039382d6d8331eec941faa8e69e5b015572eba04d83707d6c2a1d749c \
	"$rootfs/usr/bin/bluetoothctl"
check_sha 5f367266e40e2103d64f7e448d99a856158515fc8cdb5ddcf63b5b8bf2330ec7 \
	"$rootfs/usr/lib/systemd/system/bluetooth.service"
check_sha 283ffc565402a1ed9c73faa471a91853dbaaade4aeee704c796a579e5a5c2691 \
	"$rootfs/etc/bluetooth/main.conf"
check_sha e806d40fc7bd533144b8e80ff728a480c8cad3a26700f721d61214ab2112ba31 \
	"$rootfs/usr/lib/aarch64-linux-gnu/libdbus-1.so.3.19.13"
check_sha 8045531c147ce39bf8b6e89beb85f97bb8ad4c495b8f8f610a23ebc2485bae69 \
	"$rootfs/usr/lib/aarch64-linux-gnu/libsystemd.so.0.32.0"
check_sha ea6c856fa685bbf9707c58499c417ca745c3161f8d36f61926a0a6b434d057a9 \
	"$rootfs/usr/sbin/rfkill"
check_sha 1fa69ec9546df80a561271004f933a98f7c55d708f7a01ca30ac20171af8c4a3 \
	"$rootfs/usr/bin/install"
check_sha 8004c707a071bab12c20f03d0f63d3edea59bbc4d1697ebec44b12fa58f81fea \
	"$rootfs/usr/share/dbus-1/system-services/org.bluez.service"
check_sha 3baa2eeaa43c959054687a67771e7435e73b2ff3e79dfb765121d8b7dc719391 \
	"$firmware_source/rtl_bt/rtl8821cs_fw.bin"
check_sha 6ddeb15f23588053e00cb08d25588bd7cf98d60fa93d9478efcef4ae8064a7ac \
	"$firmware_source/rtl_bt/rtl8821cs_config.bin"

bash -n "$project/src/rg40xxv-bluetooth-hci-ready"
bash -n "$project/tests/test-hci-ready.sh"
"$project/tests/test-hci-ready.sh"
"$project/tests/test-bluetooth-model.sh"
RG40XXV_WORKSPACE=$workspace RG40XXV_STOCK_ROOTFS=$rootfs \
	AARCH64_CC=$cc "$project/tests/test-helper-integration.sh"

mkdir -p "$build/repro-a" "$build/repro-b" "$stage"
find "$build/repro-a" -mindepth 1 -delete
find "$build/repro-b" -mindepth 1 -delete
find "$stage" -mindepth 1 -delete

common_flags=(
	-std=c11 -O2 -pipe -mcpu=cortex-a53 -mtune=cortex-a53
	-fPIE -fstack-protector-strong -D_FORTIFY_SOURCE=2
	-Wall -Wextra -Werror -Wformat=2 -Wshadow
	-I"$project/src"
	-I"$rootfs/usr/include/dbus-1.0"
	-I"$rootfs/usr/lib/aarch64-linux-gnu/dbus-1.0/include"
)
link_flags=(
	"$rootfs/usr/lib/aarch64-linux-gnu/libdbus-1.so.3.19.13"
	-Wl,-rpath-link,"$rootfs/usr/lib/aarch64-linux-gnu"
	-pie -Wl,-z,relro,-z,now -Wl,--as-needed -Wl,--build-id=none
)
for rebuild in repro-a repro-b; do
	"$cc" "${common_flags[@]}" \
		"$project/src/rg40xxv-bluetooth-control.c" \
		"$project/src/bluetooth_model.c" "${link_flags[@]}" \
		-o "$build/$rebuild/rg40xxv-bluetooth-control"
done
cmp "$build/repro-a/rg40xxv-bluetooth-control" \
	"$build/repro-b/rg40xxv-bluetooth-control" || die non-reproducible-helper
helper=$build/repro-a/rg40xxv-bluetooth-control
file "$helper" | grep -Fq 'ARM aarch64' || die helper-not-aarch64
readelf -h "$helper" | grep -Eq 'Type:[[:space:]]+DYN' || die helper-not-pie
readelf -l "$helper" | grep -Fq GNU_RELRO || die helper-no-relro
readelf -d "$helper" | grep -Fq BIND_NOW || die helper-no-bind-now
if readelf -d "$helper" | grep -Eq 'RPATH|RUNPATH'; then
	die helper-host-search-path
fi
needed=$(readelf -d "$helper" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' |
	LC_ALL=C sort)
expected_needed=$(printf '%s\n' ld-linux-aarch64.so.1 libc.so.6 libdbus-1.so.3 |
	LC_ALL=C sort)
[[ $needed == "$expected_needed" ]] || die helper-unexpected-needed-library
if readelf --version-info "$helper" | grep -Eq 'GLIBC_2\.(3[6-9]|[4-9][0-9])'; then
	die helper-newer-than-stock-glibc
fi

install -d -m 0755 \
	"$stage/rootfs-overlay/usr/sbin" \
	"$stage/rootfs-overlay/usr/libexec/rg40xxv" \
	"$stage/rootfs-overlay/usr/lib/firmware/rtl_bt" \
	"$stage/rootfs-overlay/usr/lib/systemd/system" \
	"$stage/rootfs-overlay/etc/systemd/system/bluetooth.service.d" \
	"$stage/rootfs-overlay/etc/systemd/system/rg40xxv-ui.service.d" \
	"$stage/rootfs-overlay/etc/systemd/system/multi-user.target.d" \
	"$stage/rootfs-overlay/etc/systemd/system" \
	"$stage/opt/rg40xxv/bluetooth/runtime" \
	"$stage/manifest"
install -m 0755 "$helper" \
	"$stage/rootfs-overlay/usr/sbin/rg40xxv-bluetooth-control"
install -m 0755 "$project/src/rg40xxv-bluetooth-hci-ready" \
	"$stage/rootfs-overlay/usr/libexec/rg40xxv/rg40xxv-bluetooth-hci-ready"
install -m 0644 "$firmware_source/rtl_bt/rtl8821cs_fw.bin" \
	"$stage/rootfs-overlay/usr/lib/firmware/rtl_bt/rtl8821cs_fw.bin"
install -m 0644 "$firmware_source/rtl_bt/rtl8821cs_config.bin" \
	"$stage/rootfs-overlay/usr/lib/firmware/rtl_bt/rtl8821cs_config.bin"
install -m 0644 "$project/payload/rg40xxv-bluetooth-state.service" \
	"$stage/rootfs-overlay/usr/lib/systemd/system/rg40xxv-bluetooth-state.service"
install -m 0644 "$project/payload/rg40xxv-bluetooth-hci-ready.service" \
	"$stage/rootfs-overlay/usr/lib/systemd/system/rg40xxv-bluetooth-hci-ready.service"
install -m 0644 "$project/payload/var-lib-bluetooth.mount" \
	"$stage/rootfs-overlay/usr/lib/systemd/system/var-lib-bluetooth.mount"
install -m 0644 "$project/payload/20-rg40xxv-runtime.conf" \
	"$stage/rootfs-overlay/etc/systemd/system/bluetooth.service.d/20-rg40xxv-runtime.conf"
install -m 0644 "$project/payload/30-bluetooth.conf" \
	"$stage/rootfs-overlay/etc/systemd/system/rg40xxv-ui.service.d/30-bluetooth.conf"
install -m 0644 "$project/payload/30-bluetooth-service.conf" \
	"$stage/rootfs-overlay/etc/systemd/system/multi-user.target.d/30-bluetooth.conf"
install -m 0644 "$project/payload/admission.env" \
	"$stage/opt/rg40xxv/bluetooth/runtime/admission.env"
install -m 0644 "$project/manifest/runtime-lock.env" \
	"$stage/manifest/runtime-lock.env"
install -m 0644 "$project/README-zh-TW.md" \
	"$stage/manifest/README-zh-TW.md"
git -C "$linux_firmware_repo" show \
	HEAD:LICENSES/LICENCE.rtlwifi_firmware.txt \
	>"$stage/manifest/LICENCE.rtlwifi_firmware.txt"
chmod 0644 "$stage/manifest/LICENCE.rtlwifi_firmware.txt"
[[ $(sha256sum "$stage/manifest/LICENCE.rtlwifi_firmware.txt" | \
	awk '{print $1}') == \
	a61351665b4f264f6c631364f85b907d8f8f41f8b369533ef4021765f9f3b62e ]] || \
	die firmware-license-hash
ln -s bluetooth.service \
	"$stage/rootfs-overlay/usr/lib/systemd/system/dbus-org.bluez.service"

"$project/tests/test-payload.sh" "$stage"
find "$stage" -exec touch -h -d "@$source_date_epoch" '{}' +
(
	cd "$stage"
	find . -type f ! -path './manifest/SHA256SUMS' -print0 |
		LC_ALL=C sort -z | xargs -0 sha256sum | sed 's#  \./#  #'
) >"$stage/manifest/SHA256SUMS"
(
	cd "$stage"
	find . -type l -print0 | LC_ALL=C sort -z |
		while IFS= read -r -d '' link; do
			printf '%s\t%s\n' "${link#./}" "$(readlink -- "$link")"
		done
) >"$stage/manifest/SYMLINKS.tsv"
touch -d "@$source_date_epoch" "$stage/manifest/SHA256SUMS" \
	"$stage/manifest/SYMLINKS.tsv"
(
	cd "$stage"
	sha256sum -c manifest/SHA256SUMS >/dev/null
)

tar --sort=name --format=gnu --mtime="@$source_date_epoch" \
	--owner=0 --group=0 --numeric-owner -C "$stage" -cJf "$archive.tmp" .
mv "$archive.tmp" "$archive"
sha256sum "$archive" >"$archive.sha256"

printf 'BLUETOOTH_RUNTIME_BUILD result=PASS helper=%s\n' \
	"$stage/rootfs-overlay/usr/sbin/rg40xxv-bluetooth-control"
printf 'ADMISSION=PASS DEVICE_VALIDATION=PENDING\n'
printf 'KERNEL_REBUILD_REQUIRED=NO DEVICE_WRITE=NONE\n'
printf 'ARCHIVE=%s\n' "$archive"
