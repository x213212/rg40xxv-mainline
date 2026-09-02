#!/usr/bin/env bash
set -euo pipefail

# Internal, manifest-driven p7 deploy helper.  The public entry point is
# tools/rg40xxv.sh.  This script never writes p8; the packaged device installer
# reads it before and after the p7 transaction and refuses any non-frozen SHA.

script=$(realpath -- "$0")
workspace=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)
artifact_input=${1:-}
device=${RG40XXV_DEVICE:-PUT-YOUR-OWN-DEVICE-HERE}
password=${RG40XXV_PASSWORD:-}

# These scripts talk to a handheld over SSH. The address and the password are
# whoever is running them, so there is no useful default: set RG40XXV_DEVICE and
# RG40XXV_PASSWORD. The refusal below is the guard working, not a bug.
case "$device" in
PUT-YOUR-OWN-DEVICE-HERE)
	printf '%s\n' 'set RG40XXV_DEVICE=user@host for your own device' >&2
	exit 2 ;;
esac
[ -n "$password" ] || { printf '%s\n' 'set RG40XXV_PASSWORD' >&2; exit 2; }
known_hosts=${RG40XXV_KNOWN_HOSTS:-$workspace/firmware/live/known_hosts}
frozen_p8_sha=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
cycle_lock=$workspace/reports/.rg40xxv-p7-cycle.lock
cycle_run_input=${RG40XXV_P7_CYCLE_RUN:-}
cycle_lock_fd=${RG40XXV_P7_CYCLE_LOCK_FD:-}

die()
{
	printf 'P7_ARTIFACT_DEPLOY result=FAIL reason=%s p8_write=NONE\n' "$1" >&2
	exit 1
}

[[ $# == 1 ]] || {
	printf 'usage: %s /absolute/path/to/ARTIFACT.env\n' "$0" >&2
	exit 2
}
for tool in awk chmod cp dirname find flock id mkdir mktemp python3 realpath rm \
	scp sed sha256sum sort ssh sshpass stat tar xargs; do
	command -v "$tool" >/dev/null 2>&1 || die "missing-tool:$tool"
done
[[ $(id -u) == 0 ]] || die host-root-required

# This helper is deliberately not a second public deploy path.  It must inherit
# the cycle lock from rg40xxv-p7-cycle.sh, and the artifact must belong to that
# exact cycle.  Taking flock on the inherited open-file description also makes
# a manually prepared invocation serialized instead of racing the public flow.
[[ $cycle_lock_fd =~ ^[0-9]+$ && $cycle_lock_fd -ge 3 ]] || \
	die cycle-lock-not-inherited
[[ -e /proc/$$/fd/$cycle_lock_fd ]] || die cycle-lock-fd-missing
[[ -f $cycle_lock && ! -L $cycle_lock ]] || die cycle-lock-path
[[ $(stat -Lc '%d:%i' "/proc/$$/fd/$cycle_lock_fd") == \
	$(stat -Lc '%d:%i' "$cycle_lock") ]] || die cycle-lock-inode
flock -n "$cycle_lock_fd" || die cycle-lock-not-held
[[ -n $cycle_run_input && ! -L $cycle_run_input ]] || die cycle-run-not-inherited
cycle_run=$(realpath -e -- "$cycle_run_input") || die cycle-run-missing
case $cycle_run in "$workspace"/reports/p7-cycles/*) ;; *) die cycle-run-path ;; esac
[[ -d $cycle_run && ! -L $cycle_run ]] || die cycle-run-not-directory

[[ ! -L $artifact_input ]] || die artifact-symlink
artifact=$(realpath -e -- "$artifact_input") || die artifact-missing
[[ -f $artifact && ! -L $artifact ]] || die artifact-not-regular
cycle_dir=${artifact%/ARTIFACT.env}
[[ $artifact == "$cycle_dir/ARTIFACT.env" ]] || die artifact-name
[[ ${cycle_dir%/*} == "$workspace/reports/p7-cycles" ]] || \
	die artifact-outside-cycle
cycle_id=${cycle_dir##*/}
[[ $cycle_id =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || \
	die artifact-cycle-id
[[ $cycle_dir == "$cycle_run" ]] || die artifact-cycle-mismatch
artifact_sidecar=$artifact.sha256
[[ -f $artifact_sidecar && ! -L $artifact_sidecar ]] || \
	die artifact-sidecar-missing
[[ $(sha256sum "$artifact") == "$(<"$artifact_sidecar")" ]] || \
	die artifact-sidecar-check
artifact_meta=$(stat -c '%u:%g:%a' "$artifact") || die artifact-stat
[[ $artifact_meta == 0:0:444 ]] || die artifact-permissions
sidecar_meta=$(stat -c '%u:%g:%a' "$artifact_sidecar") || die artifact-sidecar-stat
[[ $sidecar_meta == 0:0:444 ]] || die artifact-sidecar-permissions

env_value()
{
	local file=$1 key=$2 value
	value=$(awk -F= -v key="$key" '
		$1 == key { sub(/^[^=]*=/, ""); print; count++ }
		END { if (count != 1) exit 1 }
	' "$file") || die "artifact-key:$key"
	printf '%s' "$value"
}

[[ $(env_value "$artifact" schema) == rg40xxv-p7-artifact-v1 ]] || \
	die artifact-schema
artifact_run_id=$(env_value "$artifact" run_id)
[[ $artifact_run_id == "$cycle_id" ]] || die artifact-run-id
release=$(env_value "$artifact" release_id)
archive_raw=$(env_value "$artifact" archive_path)
archive_sha=$(env_value "$artifact" archive_sha256)
archive_bytes=$(env_value "$artifact" archive_bytes)
sidecar_raw=$(env_value "$artifact" archive_sidecar_path)
kit_raw=$(env_value "$artifact" deploy_kit_path)
kit_sha=$(env_value "$artifact" deploy_kit_tree_sha256)
bundle_manifest_raw=$(env_value "$artifact" bundle_manifest_path)
bundle_manifest_sha=$(env_value "$artifact" bundle_manifest_sha256)
build_lock_raw=$(env_value "$artifact" build_lock_path)
build_lock_sha=$(env_value "$artifact" build_lock_sha256)
artifact_p8=$(env_value "$artifact" frozen_p8_sha256)
[[ $(env_value "$artifact" p8_payload) == NONE ]] || die artifact-p8-payload
[[ $(env_value "$artifact" p8_write) == NONE ]] || die artifact-p8-write

[[ $release =~ ^[0-9a-f]{64}$ ]] || die release-id-format
[[ $archive_sha =~ ^[0-9a-f]{64}$ ]] || die archive-sha-format
[[ $archive_bytes =~ ^[1-9][0-9]*$ ]] || die archive-bytes-format
[[ $kit_sha =~ ^[0-9a-f]{64}$ ]] || die deploy-kit-sha-format
[[ $bundle_manifest_sha =~ ^[0-9a-f]{64}$ ]] || die bundle-manifest-sha-format
[[ $build_lock_sha =~ ^[0-9a-f]{64}$ ]] || die build-lock-sha-format
[[ $artifact_p8 == "$frozen_p8_sha" ]] || die artifact-p8-not-frozen

expected_archive=$cycle_dir/artifacts/rg40xxv-release-$release.tar.xz
expected_sidecar=$expected_archive.sha256
expected_kit=$cycle_dir/artifacts/deploy-kit-$release
expected_bundle_manifest=$cycle_dir/artifacts/BUNDLE-SHA256SUMS
expected_build_lock=$cycle_dir/BUILD-LOCK.env
[[ $archive_raw == "$expected_archive" && ! -L $archive_raw ]] || \
	die archive-path
[[ $sidecar_raw == "$expected_sidecar" && ! -L $sidecar_raw ]] || \
	die sidecar-path
[[ $kit_raw == "$expected_kit" && ! -L $kit_raw ]] || die deploy-kit-path
[[ $bundle_manifest_raw == "$expected_bundle_manifest" &&
   ! -L $bundle_manifest_raw ]] || die bundle-manifest-path
[[ $build_lock_raw == "$expected_build_lock" && ! -L $build_lock_raw ]] || \
	die build-lock-path
archive=$(realpath -e -- "$archive_raw") || die archive-missing
sidecar=$(realpath -e -- "$sidecar_raw") || die sidecar-missing
kit=$(realpath -e -- "$kit_raw") || die deploy-kit-missing
bundle_manifest=$(realpath -e -- "$bundle_manifest_raw") || \
	die bundle-manifest-missing
build_lock=$(realpath -e -- "$build_lock_raw") || die build-lock-missing
[[ $archive == "$expected_archive" && $sidecar == "$expected_sidecar" &&
   $kit == "$expected_kit" &&
   $bundle_manifest == "$expected_bundle_manifest" &&
   $build_lock == "$expected_build_lock" ]] || \
	die artifact-canonical-path
[[ -f $archive && ! -L $archive ]] || die archive-not-regular
[[ -f $sidecar && ! -L $sidecar ]] || die sidecar-not-regular
[[ -d $kit && ! -L $kit ]] || die deploy-kit-not-directory
[[ -f $bundle_manifest && ! -L $bundle_manifest ]] || \
	die bundle-manifest-not-regular
[[ -f $build_lock && ! -L $build_lock &&
   -f $build_lock.sha256 && ! -L $build_lock.sha256 ]] || \
	die build-lock-files
[[ $(stat -c '%u:%g:%a' "$build_lock") == 0:0:444 &&
   $(stat -c '%u:%g:%a' "$build_lock.sha256") == 0:0:444 ]] || \
	die build-lock-permissions
[[ $(sha256sum "$build_lock" | awk '{print $1}') == "$build_lock_sha" ]] || \
	die build-lock-sha
[[ $(sha256sum "$build_lock") == "$(<"$build_lock.sha256")" ]] || \
	die build-lock-sidecar
[[ ${archive##*/} == "rg40xxv-release-$release.tar.xz" ]] || \
	die archive-name
[[ ${kit##*/} == "deploy-kit-$release" ]] || die deploy-kit-name
[[ $(stat -c %s "$archive") == "$archive_bytes" ]] || die archive-bytes
[[ $(sha256sum "$archive" | awk '{print $1}') == "$archive_sha" ]] || \
	die archive-sha
[[ $(<"$sidecar") == "$archive_sha  ${archive##*/}" ]] || die sidecar-content
[[ $(<"$kit/EXPECTED_RELEASE_ID") == "$release" ]] || die kit-release
[[ $(<"$kit/EXPECTED_ARCHIVE_SHA256") == "$archive_sha" ]] || \
	die kit-archive
[[ $(sha256sum "$bundle_manifest" | awk '{print $1}') == \
	"$bundle_manifest_sha" ]] || die bundle-manifest-sha
(
	cd "${bundle_manifest%/*}"
	sha256sum -c BUNDLE-SHA256SUMS >/dev/null
) || die bundle-manifest-check
observed_kit_sha=$(
	cd "$kit"
	{
		find . -mindepth 1 -printf 'META\t%y\t%m\t%U\t%G\t%s\t%p\t%l\n' | \
			LC_ALL=C sort
		find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | \
			sed 's#  \./#FILE\t#'
	} | sha256sum | awk '{print $1}'
)
[[ $observed_kit_sha == "$kit_sha" ]] || die deploy-kit-tree-sha

# Validate the exact archive and verifier on the host before contacting the
# device.  A failed host check leaves p7 and p8 untouched.
host_check=$(mktemp -d /tmp/rg40xxv-p7-artifact-check.XXXXXX)
cleanup_host()
{
	case ${host_check:-} in
		/tmp/rg40xxv-p7-artifact-check.*) rm -rf -- "$host_check" ;;
	esac
}
trap cleanup_host EXIT
mkdir -m 0700 "$host_check/release" "$host_check/scratch"
python3 "$kit/inspect-release-archive.py" "$archive" >/dev/null
tar --extract --xz --file "$archive" --directory "$host_check/release" \
	--same-owner --same-permissions --numeric-owner \
	--delay-directory-restore --no-overwrite-dir
RG40XXV_GENERIC_VERIFIER="$kit/rg40xxv-verify-release" \
	"$kit/rg40xxv-verify-next-release" \
	"$host_check/release" "$release" "$host_check/scratch"
cleanup_host
trap - EXIT
printf 'P7_ARTIFACT_DEPLOY host_verify=PASS release=%s\n' "$release"

ssh_options=(
	-o StrictHostKeyChecking=yes
	-o UserKnownHostsFile="$known_hosts"
	-o ConnectTimeout=8
	-o ServerAliveInterval=10
	-o ServerAliveCountMax=6
)
remote()
{
	sshpass -p "$password" ssh "${ssh_options[@]}" "$device" "$@"
}

before=$(remote "dd if=/dev/mmcblk0p8 bs=4M count=16 iflag=fullblock status=none 2>/dev/null | sha256sum | cut -d' ' -f1")
[[ $before == "$frozen_p8_sha" ]] || die p8-not-frozen-before

remote_upload=/mnt/data/.rg-install
remote_archive=$remote_upload/${archive##*/}
remote_sidecar=$remote_upload/${sidecar##*/}
remote_kit=$remote_upload/${kit##*/}

cleanup_remote_upload()
{
	remote "rm -f -- '$remote_archive' '$remote_sidecar'; rm -rf -- '$remote_kit'" \
		>/dev/null 2>&1 || true
}

# Every removal target is derived from a validated 64-hex release ID.
remote "set -eu
install -d -m 0700 '$remote_upload'
rm -f -- '$remote_archive' '$remote_sidecar'
rm -rf -- '$remote_kit'"
trap 'cleanup_remote_upload; cleanup_host' EXIT
sshpass -p "$password" scp -q "${ssh_options[@]}" "$archive" "$sidecar" \
	"$device:$remote_upload/"
sshpass -p "$password" scp -q -r "${ssh_options[@]}" "$kit" \
	"$device:$remote_upload/"
remote_kit_sha=$(remote "set -eu
cd '$remote_kit'
{ find . -mindepth 1 -printf 'META\\t%y\\t%m\\t%U\\t%G\\t%s\\t%p\\t%l\\n' | LC_ALL=C sort
  find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sed 's#  \\./#FILE\\t#'
} | sha256sum | awk '{print \$1}'") || die remote-kit-tree-readback
[[ $remote_kit_sha == "$kit_sha" ]] || die remote-kit-tree-sha

set +e
remote "set -eu
chown -R 0:0 '$remote_kit'
chmod -R go-w '$remote_kit'
chown 0:0 '$remote_archive' '$remote_sidecar'
chmod 0600 '$remote_archive' '$remote_sidecar'
kit_exec=\$(mktemp -d /root/.rg40xxv-kit.XXXXXX)
cleanup() { rm -rf -- \"\$kit_exec\"; }
trap cleanup EXIT
cp -a '$remote_kit/.' \"\$kit_exec/\"
\"\$kit_exec/install-p7-only.sh\" '$remote_archive' '$remote_sidecar'"
install_rc=$?
set -e

cleanup_remote_upload
after=$(remote "dd if=/dev/mmcblk0p8 bs=4M count=16 iflag=fullblock status=none 2>/dev/null | sha256sum | cut -d' ' -f1") || \
	die p8-readback-after-unavailable
[[ $after == "$before" ]] || die p8-changed
[[ $install_rc -eq 0 ]] || die "installer-exit-$install_rc"
trap - EXIT
current=$(remote 'cat /mnt/data/rg40xxv/current.release')
[[ $current == "$release" ]] || die current-pointer
printf 'P7_ARTIFACT_DEPLOY result=PASS release=%s p8_write=NONE p8_sha256=%s\n' \
	"$release" "$after"
