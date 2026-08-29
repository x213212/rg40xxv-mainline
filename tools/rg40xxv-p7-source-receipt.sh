#!/usr/bin/env bash
set -euo pipefail

# Produce the repository-managed p7 source/input receipt consumed by the cycle
# runner and release builder. It records bytes plus namespace metadata, so an
# added/deleted file, changed symlink, or mode change invalidates the lock.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
output_input=${1:-}

die()
{
	printf 'P7_SOURCE_RECEIPT result=FAIL reason=%s\n' "$1" >&2
	exit 1
}

[[ $# == 1 ]] || {
	printf 'usage: %s EMPTY_OUTPUT_DIRECTORY\n' "$0" >&2
	exit 2
}
for tool in basename chmod dirname find mkdir mv realpath rm sha256sum sort xargs; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done

if [[ -e $output_input || -L $output_input ]]; then
	die output-already-exists
fi
output=$(realpath -m -- "$output_input") || die output-realpath
output_parent=$(dirname -- "$output")
output_name=$(basename -- "$output")
safe_output=0
if [[ $(dirname -- "$output_parent") == \
	"$workspace/reports/p7-cycles" &&
      ${output_parent##*/} =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ &&
      ( $output_name == source-receipt ||
        $output_name =~ ^\.source-receipt\.[A-Za-z0-9._-]+$ ) ]]; then
	safe_output=1
elif [[ $output_parent == "$workspace/lab/deploy/rg40xxv-next-v1/build" &&
        $output_name =~ ^\.source-receipt\.[A-Za-z0-9._-]+$ ]]; then
	safe_output=1
elif [[ $output_parent == /tmp &&
        $output_name =~ ^rg40xxv-p7-source-receipt\.[A-Za-z0-9._-]+$ ]]; then
	safe_output=1
fi
[[ $safe_output == 1 && -d $output_parent && ! -L $output_parent ]] || \
	die unsafe-output-directory

roots=(
	lab/candidates/rg40xxv-next-v1-src
	lab/deploy/rg40xxv-next-v1
	lab/emulators/aarch64-staging/stage
	lab/emulators/aarch64-staging/compat
	lab/emulators/aarch64-staging/scripts
	lab/emulators/aarch64-staging/manifest
	lab/emulators/aarch64-staging/config
	lab/emulators/aarch64-staging/tests
	lab/emulators/aarch64-staging/tooling
	lab/emulators/aarch64-staging/sources/easyrpg-player
	lab/reverse/reference-rpgmakermlinux-cicpoffs/candidate/easyrpg-h700/prepared-games
	lab/reverse/reference-rpgmakermlinux-cicpoffs/candidate/easyrpg-h700/logs/content-smoke/screenshots
	build-inputs/rg40xxv-test1-firmware/rtl_bt
	firmware/mnt/rootfs/usr/include
	firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu
	firmware/mnt/rootfs/usr/share/glvnd
	services/netstream/src
	services/netstream/include
)
explicit=(
	firmware/mnt/rootfs/lib
	firmware/mnt/rootfs/etc/ld.so.cache
	firmware/mnt/rootfs/etc/bluetooth/main.conf
	firmware/mnt/rootfs/usr/bin/bluetoothctl
	firmware/mnt/rootfs/usr/bin/install
	firmware/mnt/rootfs/usr/libexec/bluetooth/bluetoothd
	firmware/mnt/rootfs/usr/lib/systemd/system/bluetooth.service
	firmware/mnt/rootfs/usr/sbin/rfkill
	firmware/mnt/rootfs/usr/share/dbus-1/system-services/org.bluez.service
	references/linux-firmware/.git/HEAD
	references/linux-firmware/.git/packed-refs
	references/linux-firmware/.git/refs/heads/main
	tools/rg40xxv-p7-ui-pipeline.sh
	tools/run-p7-component-host-gate.sh
	tools/rg40xxv-p7-source-receipt.sh
	tools/rg40xxv-p7-cycle.sh
	tools/deploy-rg40xxv-p7-artifact.sh
	tools/rg40xxv-p7-device-acceptance.sh
	tools/rg40xxv.sh
	tools/tests/test-rg40xxv-pipeline-static.sh
	references/rg40xxv-release-workflow.md
	reports/rg40xxv-p7-ui-build-test-guide-20260829.md
	skills/rg40xxv-p7/SKILL.md
	lab/deploy/rg40xxv-production-v1/boot/rg40xxv-verify-release
	lab/deploy/rg40xxv-production-v1/boot/init
	lab/deploy/rg40xxv-production-v1/installer/inspect-release-archive.py
	lab/deploy/rg40xxv-production-v1/installer/materialize-release.py
	lab/deploy/rg40xxv-production-v1/installer/install-release.sh
	lab/deploy/rg40xxv-production-v1/installer/test-materializer.sh
	lab/deploy/rg40xxv-production-v1/tests/test-boot-userspace.sh
	lab/deploy/rg40xxv-production-v1/tests/test-exit-chord-supervisor.sh
	lab/deploy/rg40xxv-production-v1/payload/rg40xxv-boot-health
	lab/deploy/rg40xxv-production-v1/payload/rg40xxv-reboot-target
	lab/tools/rg40xxv-import-stock-saves.py
	firmware/mnt/rootfs/usr/lib/firmware/regulatory.db
	firmware/mnt/rootfs/usr/lib/firmware/regulatory.db.p7s
	firmware/mnt/rootfs/usr/share/zoneinfo/Asia/Taipei
	firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libasound.so
	firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libSDL2_ttf.so
	firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libSDL2_image.so
	firmware/mnt/rootfs/usr/lib/aarch64-linux-gnu/libSDL2.so
)

staging=$output_parent/.${output_name}.tmp.$$
[[ ! -e $staging && ! -L $staging ]] || die staging-already-exists
cleanup_receipt()
{
	case ${staging:-} in
		"$output_parent"/.${output_name}.tmp.*) rm -rf -- "$staging" ;;
	esac
}
trap cleanup_receipt EXIT
mkdir -m 0700 -- "$staging"
[[ $(realpath -e -- "$staging") == "$staging" ]] || die staging-realpath

path_list=$staging/.paths
link_list=$staging/.links
tree=$staging/SOURCE-TREE.tsv
hashes=$staging/SOURCE-SHA256SUMS

(
	cd "$workspace"
	for root in "${roots[@]}"; do
		[[ -d $root && ! -L $root ]] || die "source-root:$root"
	done
	for path in "${explicit[@]}"; do
		[[ -e $path || -L $path ]] || die "source-input:$path"
	done

	{
		for root in "${roots[@]}"; do
			find "$root" \
				\( -type d \( -name build -o -name __pycache__ -o -name .git \) \) \
				-prune -o \( -type d -o -type f -o -type l \) -print0
		done
		printf '%s\0' "${explicit[@]}"
	} | LC_ALL=C sort -zu >"$path_list"
	while IFS= read -r -d '' path; do
		case $path in *$'\n'*|*$'\t'*) die unsafe-source-path ;; esac
	done <"$path_list"

	{
		for root in "${roots[@]}"; do
			find "$root" \
				\( -type d \( -name build -o -name __pycache__ -o -name .git \) \) \
				-prune -o -type l -printf '%l\0'
		done
		find "${explicit[@]}" -maxdepth 0 -type l -printf '%l\0'
	} >"$link_list"
	while IFS= read -r -d '' target; do
		case $target in *$'\n'*|*$'\t'*) die unsafe-symlink-target ;; esac
	done <"$link_list"

	{
		for root in "${roots[@]}"; do
			find "$root" \
				\( -type d \( -name build -o -name __pycache__ -o -name .git \) \) \
				-prune -o \
				\( -type f -printf 'f\t%m\t%U\t%G\t%s\t%p\n' -o \
				   -type d -printf 'd\t%m\t%U\t%G\t-\t%p\n' -o \
				   -type l -printf 'l\t%m\t%U\t%G\t-\t%p\t%l\n' \)
		done
		find "${explicit[@]}" -maxdepth 0 \
			\( -type f -printf 'f\t%m\t%U\t%G\t%s\t%p\n' -o \
			   -type d -printf 'd\t%m\t%U\t%G\t-\t%p\n' -o \
			   -type l -printf 'l\t%m\t%U\t%G\t-\t%p\t%l\n' \)
	} | LC_ALL=C sort -u >"$tree"

	{
		for root in "${roots[@]}"; do
			find "$root" \
				\( -type d \( -name build -o -name __pycache__ -o -name .git \) \) \
				-prune -o -type f -print0
		done
		find "${explicit[@]}" -maxdepth 0 -type f -print0
	} | LC_ALL=C sort -zu | xargs -0 -r sha256sum -- >"$hashes"
)

rm -f -- "$path_list" "$link_list"
chmod 0444 "$tree" "$hashes"
mv -T -n -- "$staging" "$output"
[[ ! -e $staging && -d $output && ! -L $output ]] || die output-publish-race
staging=
trap - EXIT
printf 'P7_SOURCE_RECEIPT result=PASS tree=%s hashes=%s\n' \
	"$output/SOURCE-TREE.tsv" "$output/SOURCE-SHA256SUMS"
