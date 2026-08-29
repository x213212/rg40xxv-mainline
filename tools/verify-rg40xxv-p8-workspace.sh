#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
FULL=0
REGISTRY="$ROOT/tools/rg40xxv-p8-images.tsv"
V9_INITRAMFS="$ROOT/builds/rg40xxv-production-7.2/panel-adopt-closeflow-v9-noquiesce/usr/initramfs_data.cpio"

case ${1-} in
	"") ;;
	--full) FULL=1 ;;
	*) printf 'usage: %s [--full]\n' "$0" >&2; exit 2 ;;
esac

check_sha()
{
	expected=$1
	path=$2
	[ -f "$path" ] || {
		printf 'P8_WORKSPACE_VERIFY result=FAIL reason=missing path=%s\n' "$path" >&2
		exit 1
	}
	actual=$(sha256sum "$path" | awk '{print $1}')
	[ "$actual" = "$expected" ] || {
		printf 'P8_WORKSPACE_VERIFY result=FAIL reason=sha256 path=%s actual=%s expected=%s\n' \
			"$path" "$actual" "$expected" >&2
		exit 1
	}
	printf 'SHA256_OK %s %s\n' "$expected" "$path"
}

check_sha \
	6fb84a821f4906215b14d19f21760a50491eb41283eb18a44991c87d2fff066e \
	"$ROOT/builds/rg40xxv-production-7.2/panel-adopt-closeflow-v9-noquiesce/arch/arm64/boot/Image"
check_sha \
	9ff8c5fc93d1992a1acddae6ae37608ebf6bafb4d00c78240dd396b7dd7c1bf9 \
	"$ROOT/builds/rg40xxv-production-7.2/panel-adopt-closeflow-v9-noquiesce/usr/initramfs_data.cpio"
check_sha \
	3a5c0b5efb9e181628826fec46b3e333e6aa931d6d85550830f8d890f4800ee4 \
	"$ROOT/lab/candidates/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2.dtb"
check_sha \
	6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3 \
	"$ROOT/lab/candidates/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2/rg40xxv-panel-adopt-closeflow-v9-noquiesce-dram1v2-persistent-legacy-p8.img"
check_sha \
	6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3 \
	"$ROOT/backups/rg40xxv-p8-before-v10-20260829/p8-before-1c2366398314.img"
check_sha \
	717b025f4a6a9d03e325c80b7565ceb530f781f6cda41aaa57c88504cb42dcbe \
	"$ROOT/lab/candidates/rg40xxv-p8-always-boot-v1/rg40xxv-p8-always-boot-v1-persistent-legacy-p8.img"
check_sha \
	147094f1d508cde32408f1ff0ef3cb80d7ec4ca89999069f2ec0df590cb50582 \
	"$ROOT/lab/candidates/rg40xxv-display-lifecycle-v10/artifacts/rg40xxv-display-lifecycle-v10-persistent-legacy-p8.img"
check_sha \
	1c2366398314abb28785f13f69b733f9ce67129c729f523fbcd21cf996358941 \
	"$ROOT/lab/candidates/rg40xxv-display-lifecycle-v10/failed-1c2366398314/rg40xxv-display-lifecycle-v10-persistent-legacy-p8.img"
check_sha \
	c7beb8c9958e0d359978ae36cdd43def81ad1d843138b87fe4d44cac419ba32b \
	"$ROOT/sources/linux-7.2/include/linux/clk/sun8i-de2.h"
check_sha \
	9aea67d715df9ae28a7e9d7fbbf17b78344f54a6e85782a4a88fecb905c3834c \
	"$ROOT/lab/p8-profiles/rcq-after-fw-scanout-v1/SPEC"
check_sha \
	9e335af033a1333e79a3b49b0b3c207be3f684692e53b3a8f8c8f9b4e52bff12 \
	"$ROOT/lab/p8-profiles/rcq-after-fw-scanout-v1/prepare.sh"
check_sha \
	664c35699929443884042b0b15883cf71ab50a4ca74da5a5db86a3cc8d1e6452 \
	"$ROOT/lab/p8-profiles/rcq-after-fw-scanout-v1/rcq-after-fw-scanout-v1.patch"

command -v cpio >/dev/null 2>&1 || {
	printf 'P8_WORKSPACE_VERIFY result=FAIL reason=missing-cpio\n' >&2
	exit 1
}
selector_tmp=$(mktemp)
trap 'rm -f "$selector_tmp"' EXIT HUP INT TERM
cpio -i --to-stdout sbin/rg40xxv-boot-selector \
	<"$V9_INITRAMFS" >"$selector_tmp" 2>/dev/null
check_sha \
	0726fdf02e786be70d44e00f8f22a2f3bb52b6a0e6a17ec3492d80b17d5ca0e2 \
	"$selector_tmp"
rm -f "$selector_tmp"
trap - EXIT HUP INT TERM

check_registry_policy()
{
	sha=$1
	policy=$2
	identity=$3
	row=$(awk -F '\t' -v sha="$sha" '$1 == sha {print $2 "\t" $3 "\t" $4}' \
		"$REGISTRY")
	expected=$(printf 'KNOWN\t%s\t%s' "$policy" "$identity")
	[ "$row" = "$expected" ] || {
		printf 'P8_WORKSPACE_VERIFY result=FAIL reason=registry sha256=%s expected_policy=%s expected_identity=%s actual=%s\n' \
			"$sha" "$policy" "$identity" "$row" >&2
		exit 1
	}
	printf 'P8_ARTIFACT sha256=%s deployment_status=%s identity=%s\n' \
		"$sha" "$policy" "$identity"
}

[ -f "$REGISTRY" ] && [ ! -L "$REGISTRY" ] || {
	printf 'P8_WORKSPACE_VERIFY result=FAIL reason=image-registry\n' >&2
	exit 1
}
check_registry_policy \
	6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3 \
	FROZEN panel-adopt-closeflow-v9
check_registry_policy \
	147094f1d508cde32408f1ff0ef3cb80d7ec4ca89999069f2ec0df590cb50582 \
	HOST_ONLY display-lifecycle-v10-corrected
check_registry_policy \
	1c2366398314abb28785f13f69b733f9ce67129c729f523fbcd21cf996358941 \
	BANNED display-lifecycle-v10-enodata-fail

if [ "$FULL" -eq 1 ]; then
	"$ROOT/lab/deploy/rg40xxv-production-v1/boot/tests/test-boot.sh"
	"$ROOT/lab/candidates/rg40xxv-display-lifecycle-v10/verify-host.sh"
	"$ROOT/tools/tests/test-rg40xxv-p8-cycle.sh"
	"$ROOT/tools/tests/test-arm-rg40xxv-tf1-readonly-static.sh"
	"$ROOT/tools/tests/test-identify-rg40xxv-p8-wsl-static.sh"
	"$ROOT/tools/tests/test-flash-rg40xxv-p8-wsl-static.sh"
	"$ROOT/tools/tests/test-rg40xxv-pipeline-static.sh"
fi

printf 'P8_WORKSPACE_VERIFY result=PASS full=%s device_write=NONE device_state=UNREAD\n' "$FULL"
