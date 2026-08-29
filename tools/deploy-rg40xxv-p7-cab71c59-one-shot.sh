#!/usr/bin/env bash
set -euo pipefail

# One-shot, p7-only deployment.  The release, archive and frozen p8 are pinned.
# The archive is fully extracted and verified on the host before the device is
# changed.  It is then streamed directly into a new p7 transaction directory,
# verified again on-device, and only then atomically selected.

release=cab71c59e8c5a4c42220f856f8acb6c09bbc1a188ba2cdcdf2a16f7219c0cc59
archive_sha=b07a0ad3afbe9b526ef0853fd9c142d4bbfac0121cb17eb9413e91a0b61688e9
p8_sha=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
device=${RG40XXV_DEVICE:-root@192.168.0.125}
password=${RG40XXV_PASSWORD:-root}
known_hosts=${RG40XXV_KNOWN_HOSTS:-${KERNEL_LAB_ROOT}/firmware/live/known_hosts}
workspace=${KERNEL_LAB_ROOT}
build=$workspace/lab/deploy/rg40xxv-next-v1/build
archive=$build/rg40xxv-release-$release.tar.xz
sidecar=$archive.sha256
kit=$build/deploy-kit-$release

ssh_opts=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$known_hosts" \
          -o ConnectTimeout=8 -o ServerAliveInterval=10 -o ServerAliveCountMax=6)

die()
{
	printf 'P7_ONE_SHOT result=FAIL reason=%s p8_write=NONE\n' "$1" >&2
	exit 1
}

[[ $(id -u) == 0 ]] || die host-root-required
[[ -f $archive && ! -L $archive ]] || die archive-missing
[[ -f $sidecar && ! -L $sidecar ]] || die sidecar-missing
[[ -d $kit && ! -L $kit ]] || die deploy-kit-missing
[[ $(sha256sum "$archive" | awk '{print $1}') == "$archive_sha" ]] || \
	die archive-sha-mismatch
[[ $(cat "$sidecar") == "$archive_sha  ${archive##*/}" ]] || \
	die sidecar-mismatch
[[ $(cat "$kit/EXPECTED_RELEASE_ID") == "$release" ]] || die kit-release-mismatch
[[ $(cat "$kit/EXPECTED_ARCHIVE_SHA256") == "$archive_sha" ]] || \
	die kit-archive-mismatch

# Fail before contacting the device if any archived file, mode or contract is
# inconsistent.  This deliberately costs one local extraction, not one failed
# device deployment.
host_check=$(mktemp -d /tmp/rg40xxv-p7-host-check.XXXXXX)
cleanup_host()
{
	rm -rf -- "$host_check"
}
trap cleanup_host EXIT
mkdir -m 0700 "$host_check/release" "$host_check/scratch"
tar --extract --xz --file "$archive" --directory "$host_check/release" \
	--same-owner --same-permissions --numeric-owner \
	--delay-directory-restore --no-overwrite-dir
"$kit/rg40xxv-verify-next-release" \
	"$host_check/release" "$release" "$host_check/scratch"
rm -rf -- "$host_check"
trap - EXIT
printf 'P7_ONE_SHOT host_archive_verify=PASS release=%s\n' "$release"

remote()
{
	sshpass -p "$password" ssh "${ssh_opts[@]}" "$device" "$@"
}

before=$(remote "dd if=/dev/mmcblk0p8 bs=4M count=16 iflag=fullblock 2>/dev/null | sha256sum | cut -d' ' -f1")
[[ $before == "$p8_sha" ]] || die p8-not-frozen-before

# Remove only the redundant uploaded copy; the host archive remains pinned and
# is streamed into tar, so p7 never needs archive + incoming space together.
remote "rm -f /mnt/data/.rg-install/${archive##*/} /mnt/data/.rg-install/${sidecar##*/}"
sshpass -p "$password" scp -q -r "${ssh_opts[@]}" "$kit" \
	"$device:/mnt/data/.rg-install/"

incoming=$(remote "mktemp -d /mnt/data/rg40xxv/.incoming-$release.oneshot.XXXXXX")
case $incoming in
/mnt/data/rg40xxv/.incoming-$release.oneshot.*) ;;
*) die unsafe-incoming-path ;;
esac

cleanup_remote()
{
	remote "case '$incoming' in /mnt/data/rg40xxv/.incoming-$release.oneshot.*) rm -rf -- '$incoming' ;; esac" || true
}
trap 'cleanup_remote; cleanup_host' EXIT

# Compressed bytes cross the network once and are never stored as a second p7
# copy.  current.release is still untouched here.
sshpass -p "$password" ssh "${ssh_opts[@]}" "$device" \
	"tar --extract --xz --file - --directory '$incoming' --same-owner --same-permissions --numeric-owner --delay-directory-restore --no-overwrite-dir" \
	<"$archive"

remote "set -euo pipefail
root=/mnt/data/rg40xxv
kit=/mnt/data/.rg-install/deploy-kit-$release
# p7 and /run are mounted noexec; run the kit from the (volatile) root overlay.
kit_exec=\$(mktemp -d /root/.rg40xxv-kit.XXXXXX)
cp -a \"\$kit/.\" \"\$kit_exec/\"
incoming='$incoming'
destination=\$root/releases/$release
scratch=\$(mktemp -d \$root/.verify-one-shot.XXXXXX)
cleanup() { rm -rf -- \"\$scratch\" \"\$kit_exec\"; }
trap cleanup EXIT
test ! -e \"\$destination\"
# /mnt/data is mounted noexec: run the kit verifier through the shell.
\"\$kit_exec/rg40xxv-verify-next-release\" \"\$incoming\" '$release' \"\$scratch\"
sync -f \"\$incoming\"
# mktemp -d creates 0700; the installer-made release directories are 0755.
chmod 0755 \"\$incoming\"
mv -- \"\$incoming\" \"\$destination\"
old_id=\$(cat \"\$root/current.release\")
printf '%s\\n' \"\$old_id\" >\"\$root/.previous.release.oneshot\"
printf '%s\\n' '$release' >\"\$root/.current.release.oneshot\"
chmod 0600 \"\$root/.previous.release.oneshot\" \"\$root/.current.release.oneshot\"
sync -f \"\$root/.previous.release.oneshot\"
sync -f \"\$root/.current.release.oneshot\"
mv -f \"\$root/.previous.release.oneshot\" \"\$root/previous.release\"
mv -f \"\$root/.current.release.oneshot\" \"\$root/current.release\"
sync -f \"\$root\"
rm -rf -- \"\$scratch\"
trap - EXIT
printf 'P7_ONE_SHOT device_install=PASS current=%s previous=%s\\n' '$release' \"\$old_id\""

trap - EXIT
after=$(remote "dd if=/dev/mmcblk0p8 bs=4M count=16 iflag=fullblock 2>/dev/null | sha256sum | cut -d' ' -f1")
[[ $after == "$before" ]] || die p8-changed
current=$(remote 'cat /mnt/data/rg40xxv/current.release')
[[ $current == "$release" ]] || die current-pointer-mismatch
printf 'P7_ONE_SHOT result=PASS release=%s p8_write=NONE p8_sha256=%s\n' \
	"$release" "$after"
