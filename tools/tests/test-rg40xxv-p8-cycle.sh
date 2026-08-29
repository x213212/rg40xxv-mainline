#!/usr/bin/env bash
set -euo pipefail

# Host-only contract test.  The cycle is copied into an isolated workspace and
# every common device-writing command is replaced by a failing sentinel.

script=$(realpath -- "$0")
source_cycle=$(CDPATH= cd -- "$(dirname -- "$script")/.." && pwd -P)/rg40xxv-p8-cycle.sh
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
workspace=$scratch/workspace
cycle=$workspace/tools/rg40xxv-p8-cycle.sh
sentinel=$scratch/device-command-called
real_path=$PATH

fail()
{
	printf 'P8_CYCLE_TEST result=FAIL reason=%s\n' "$1" >&2
	exit 1
}

expect_fail()
{
	local label=$1
	shift
	if "$@" >"$scratch/$label.out" 2>&1; then
		fail "unexpected-pass:$label"
	fi
}

value()
{
	local file=$1 key=$2
	awk -F= -v key="$key" '$1 == key { sub(/^[^=]*=/, ""); print }' "$file"
}

mkdir -p "$workspace/tools" "$workspace/lab/p8-profiles/good" \
	"$workspace/reports" "$scratch/fakebin"
cp -- "$source_cycle" "$cycle"
chmod 0755 "$cycle"

for command_name in dd blockdev sfdisk mount umount fastboot ssh scp; do
	{
		printf '#!/bin/sh\n'
		printf 'printf "%%s\\n" "$0" >>"$RG40XXV_DEVICE_SENTINEL"\n'
		printf 'exit 97\n'
	} >"$scratch/fakebin/$command_name"
	chmod 0755 "$scratch/fakebin/$command_name"
done
export RG40XXV_DEVICE_SENTINEL=$sentinel
export PATH=$scratch/fakebin:$real_path

printf 'single-variable fixture\n' >"$workspace/lab/p8-profiles/good/change.patch"
change_sha=$(sha256sum "$workspace/lab/p8-profiles/good/change.patch" | awk '{print $1}')

cat >"$workspace/lab/p8-profiles/good/prepare.sh" <<'PREPARE'
#!/usr/bin/env bash
set -euo pipefail
run=$1
spec=$2
[[ $run == /* && $spec == "$run/profile/SPEC" ]]
mkdir -m 0755 "$run/artifact"
printf 'kernel-image\n' >"$run/artifact/Image"
printf 'device-tree\n' >"$run/artifact/board.dtb"
printf 'initramfs\n' >"$run/artifact/initramfs_data.cpio"
printf 'selector\n' >"$run/artifact/rg40xxv-boot-selector"
temporary=$run/artifact/.p8.img
truncate -s 67108864 "$temporary"
p8_sha=$(sha256sum "$temporary" | awk '{print $1}')
mv -- "$temporary" "$run/artifact/$p8_sha.img"
image_sha=$(sha256sum "$run/artifact/Image" | awk '{print $1}')
dtb_sha=$(sha256sum "$run/artifact/board.dtb" | awk '{print $1}')
initramfs_sha=$(sha256sum "$run/artifact/initramfs_data.cpio" | awk '{print $1}')
selector_sha=$(sha256sum "$run/artifact/rg40xxv-boot-selector" | awk '{print $1}')
result=$run/.STAGED.env.$$
{
	printf 'schema=rg40xxv-p8-staged-v1\n'
	printf 'image=artifact/Image\nimage_sha256=%s\n' "$image_sha"
	printf 'dtb=artifact/board.dtb\ndtb_sha256=%s\n' "$dtb_sha"
	printf 'initramfs=artifact/initramfs_data.cpio\ninitramfs_sha256=%s\n' "$initramfs_sha"
	printf 'selector=artifact/rg40xxv-boot-selector\nselector_sha256=%s\n' "$selector_sha"
	printf 'p8=artifact/%s.img\np8_sha256=%s\np8_bytes=67108864\n' "$p8_sha" "$p8_sha"
	printf 'reproducible_builds=PASS\nhost_gate=PASS\ndevice_boot=NOT_TESTED\n'
} >"$result"
mv -- "$result" "$run/STAGED.env"
PREPARE
chmod 0755 "$workspace/lab/p8-profiles/good/prepare.sh"
prepare_sha=$(sha256sum "$workspace/lab/p8-profiles/good/prepare.sh" | awk '{print $1}')
cat >"$workspace/lab/p8-profiles/good/SPEC" <<EOF
schema=rg40xxv-p8-profile-v1
profile_id=good
base_p8_sha256=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
observable_variable_id=fixture-only
change_set=change.patch
change_set_sha256=$change_sha
prepare_script=prepare.sh
prepare_script_sha256=$prepare_sha
EOF

cp -a "$workspace/lab/p8-profiles/good" "$workspace/lab/p8-profiles/wrong-base"
sed -i \
	-e 's/profile_id=good/profile_id=wrong-base/' \
	-e 's/base_p8_sha256=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3/base_p8_sha256=09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519/' \
	"$workspace/lab/p8-profiles/wrong-base/SPEC"

locked()
(
	exec 9<>"$workspace/reports/.rg40xxv-p8-operation.lock"
	flock -n 9 || exit 96
	RG40XXV_P8_OPERATION_LOCK_FD=9 "$cycle" "$@"
)

make_manifest()
{
	local run=$1 lock lock_sha p8 p8_sha identify_dir identify_rel identify_sha
	local outgoing=6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3
	lock=$run/BUILD-LOCK.env
	lock_sha=$(awk '{print $1}' "$lock.sha256")
	p8=$(value "$lock" p8)
	p8_sha=$(value "$lock" p8_sha256)
	identify_dir=$workspace/reports/p8-identify/${run##*/}
	identify_rel=reports/p8-identify/${run##*/}/IDENTIFY.env
	mkdir -p "$identify_dir"
	cat >"$workspace/$identify_rel" <<EOF
schema=rg40xxv-p8-identify-receipt-v2
created_utc=2026-08-29T12:00:00Z
device=/dev/sdz
disk_kernel_name=sdz
disk_node_id=8:90
disk_device_number=8:144
disk_sysfs_path=/sys/devices/mock/block/sdz
diskseq=4242
disk_bytes=62516101120
logical_block_bytes=512
disk_read_only=1
gpt_guid=PUT-YOUR-OWN-CARD-GPT-GUID-HERE
gpt_dump_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
partition_count=8
p1_offset=37748736
p1_bytes=47244640256
p2_offset=47282388992
p2_bytes=33554432
p3_offset=47315943424
p3_bytes=16777216
p4_offset=47332720640
p4_bytes=67108864
p5_offset=47399829504
p5_bytes=7516192768
p6_offset=54916022272
p6_bytes=4294967296
p7_offset=59210989568
p7_bytes=2679111680
p8_offset=61890101248
p8_bytes=67108864
all_partitions_unmounted=PASS
p4_sha256=09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519
p8_sha256=$outgoing
p8_registry_outgoing_policy=KNOWN
p8_registry_target_policy=FROZEN
p8_registry_identity=fixture-v9
image_registry_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
device_write=NONE
p7_write=NONE
EOF
	chmod 0444 "$workspace/$identify_rel"
	identify_sha=$(sha256sum "$workspace/$identify_rel" | awk '{print $1}')
	cat >"$run/FLASH-MANIFEST.env" <<EOF
schema=rg40xxv-p8-flash-manifest-v2
run_id=${run##*/}
action=FLASH_REQUESTED
build_lock_sha256=$lock_sha
p8=$p8
p8_sha256=$p8_sha
p8_bytes=67108864
device=/dev/sdz
disk_node_id=8:90
disk_device_number=8:144
disk_sysfs_path=/sys/devices/mock/block/sdz
diskseq=4242
identify_receipt=$identify_rel
identify_receipt_sha256=$identify_sha
outgoing_p8_sha256=$outgoing
p7_write=NONE
device_write=NOT_PERFORMED
EOF
}

publish_flash_fixture()
{
	local run=$1 p8_sha device outgoing
	p8_sha=$(value "$run/BUILD-LOCK.env" p8_sha256)
	device=$(value "$run/FLASH-AUTHORIZATION.env" device)
	outgoing=$(value "$run/FLASH-AUTHORIZATION.env" outgoing_p8_sha256)
	cat >"$run/FLASH-RESULT.env" <<EOF
schema=rg40xxv-p8-flash-result-v2
run_id=${run##*/}
device=$device
disk_node_id=8:90
disk_device_number=8:144
disk_sysfs_path=/sys/devices/mock/block/sdz
diskseq=4242
outgoing_p8_sha256=$outgoing
p8_sha256=$p8_sha
readback_sha256=$p8_sha
p8_bytes=67108864
result=PASS
write_mode=P8_ONLY
p4=UNCHANGED
p7_write=NONE
device_write=P8_ONLY
EOF
	chmod 0444 "$run/FLASH-RESULT.env"
	(cd "$run" && sha256sum FLASH-RESULT.env >FLASH-RESULT.env.sha256)
	chmod 0444 "$run/FLASH-RESULT.env.sha256"
}

expect_fail missing-inherited-lock env -u RG40XXV_P8_OPERATION_LOCK_FD \
	"$cycle" prepare good
grep -q 'p8-operation-lock-inheritance-required' "$scratch/missing-inherited-lock.out" || \
	fail missing-lock-diagnostic
expect_fail non-frozen-profile-base locked prepare wrong-base
grep -q 'profile-base-must-be-frozen-v9' "$scratch/non-frozen-profile-base.out" || \
	fail non-frozen-base-diagnostic

prepare_output=$(locked prepare good)
run_id=$(printf '%s\n' "$prepare_output" | sed -n \
	's/.*result=PASS action=prepare run_id=\([^ ]*\).*/\1/p')
[[ $run_id =~ ^[0-9]{8}T[0-9]{6}[+-][0-9]{4}-[0-9]+$ ]] || fail prepare-run-id
run=$workspace/reports/p8-runs/$run_id
[[ $(value "$run/STATE.env" state) == PACKAGED_HOST_PASS ]] || fail prepare-state
[[ $(stat -c %a "$run/BUILD-LOCK.env") == 444 && \
	$(stat -c %a "$run/BUILD-LOCK.env.sha256") == 444 && \
	$(stat -c %a "$run/COMPONENTS.sha256") == 444 ]] || fail immutable-build-receipts
(cd "$run" && sha256sum -c BUILD-LOCK.env.sha256 >/dev/null && \
	sha256sum -c COMPONENTS.sha256 >/dev/null) || fail build-receipts

expect_fail latest-forbidden "$cycle" status latest
expect_fail observe-before-flash locked observe "$run_id" first-cold \
	"$(value "$run/BUILD-LOCK.env" p8_sha256)" PASS
expect_fail preflight-without-manifest locked flash-preflight "$run_id"
[[ $(value "$run/STATE.env" state) == PACKAGED_HOST_PASS ]] || fail failed-transition-mutated-state

make_manifest "$run"
sed -i 's#device=/dev/sdz#device=/dev/sdy#' "$run/FLASH-MANIFEST.env"
expect_fail swapped-device-reuse locked flash-preflight "$run_id"
grep -q 'identify-receipt-identity-mismatch' "$scratch/swapped-device-reuse.out" || \
	fail swapped-device-diagnostic
sed -i 's#device=/dev/sdy#device=/dev/sdz#' "$run/FLASH-MANIFEST.env"
sed -i 's/^diskseq=4242$/diskseq=4243/' "$run/FLASH-MANIFEST.env"
expect_fail stale-diskseq-reuse locked flash-preflight "$run_id"
grep -q 'flash-manifest-device-binding-mismatch' "$scratch/stale-diskseq-reuse.out" || \
	fail stale-diskseq-diagnostic
sed -i 's/^diskseq=4243$/diskseq=4242/' "$run/FLASH-MANIFEST.env"
sed -i 's#^disk_sysfs_path=/sys/devices/mock/block/sdz$#disk_sysfs_path=/sys/devices/mock/block/sdy#' \
	"$run/FLASH-MANIFEST.env"
expect_fail swapped-sysfs-reuse locked flash-preflight "$run_id"
grep -q 'flash-manifest-device-binding-mismatch' "$scratch/swapped-sysfs-reuse.out" || \
	fail swapped-sysfs-diagnostic
sed -i 's#^disk_sysfs_path=/sys/devices/mock/block/sdy$#disk_sysfs_path=/sys/devices/mock/block/sdz#' \
	"$run/FLASH-MANIFEST.env"
sed -i 's/^disk_node_id=8:90$/disk_node_id=8:91/' "$run/FLASH-MANIFEST.env"
expect_fail swapped-node-id-reuse locked flash-preflight "$run_id"
grep -q 'flash-manifest-device-binding-mismatch' "$scratch/swapped-node-id-reuse.out" || \
	fail swapped-node-id-diagnostic
sed -i 's/^disk_node_id=8:91$/disk_node_id=8:90/' "$run/FLASH-MANIFEST.env"
sed -i 's/^disk_device_number=8:144$/disk_device_number=8:145/' "$run/FLASH-MANIFEST.env"
expect_fail swapped-device-number-reuse locked flash-preflight "$run_id"
grep -q 'flash-manifest-device-binding-mismatch' \
	"$scratch/swapped-device-number-reuse.out" || fail swapped-device-number-diagnostic
sed -i 's/^disk_device_number=8:145$/disk_device_number=8:144/' "$run/FLASH-MANIFEST.env"
locked flash-preflight "$run_id" >/dev/null
[[ $(value "$run/STATE.env" state) == FLASH_AUTHORIZED ]] || fail authorization-state
[[ $(value "$run/FLASH-AUTHORIZATION.env" diskseq) == 4242 && \
	$(value "$run/FLASH-AUTHORIZATION.env" disk_node_id) == 8:90 && \
	$(value "$run/FLASH-AUTHORIZATION.env" disk_device_number) == 8:144 && \
	$(value "$run/FLASH-AUTHORIZATION.env" disk_sysfs_path) == /sys/devices/mock/block/sdz ]] || \
	fail authorization-device-binding
authorization_sha=$(sha256sum "$run/FLASH-AUTHORIZATION.env")
expect_fail repeated-preflight locked flash-preflight "$run_id"
[[ $(sha256sum "$run/FLASH-AUTHORIZATION.env") == "$authorization_sha" ]] || \
	fail authorization-clobbered
expect_fail observe-before-readback locked observe "$run_id" first-cold \
	"$(value "$run/BUILD-LOCK.env" p8_sha256)" PASS
expect_fail flash-complete-without-receipt locked flash-complete "$run_id"
[[ $(value "$run/STATE.env" state) == FLASH_AUTHORIZED ]] || fail incomplete-flash-mutated-state

publish_flash_fixture "$run"
locked flash-complete "$run_id" >/dev/null
p8_sha=$(value "$run/BUILD-LOCK.env" p8_sha256)
expect_fail out-of-order-first locked observe "$run_id" warm-1 "$p8_sha" PASS
locked observe "$run_id" first-cold "$p8_sha" PASS >/dev/null
first_record_sha=$(sha256sum "$run/observations/01-first-cold.env")
expect_fail repeated-observation locked observe "$run_id" first-cold "$p8_sha" PASS
[[ $(sha256sum "$run/observations/01-first-cold.env") == "$first_record_sha" ]] || \
	fail observation-clobbered
expect_fail skipped-warm locked observe "$run_id" warm-2 "$p8_sha" PASS
locked observe "$run_id" warm-1 "$p8_sha" PASS >/dev/null
locked observe "$run_id" warm-2 "$p8_sha" FAIL >/dev/null
[[ $(value "$run/STATE.env" state) == RECOVERY_REQUIRED && \
	$(value "$run/RECOVERY-REQUIRED.env" required_recovery_p8_sha256) == \
	6455c4d82d1594c03ab4f799c276da7249c6ffefd6fa22dd55b9077b1dfd4be3 ]] || \
	fail recovery-required-state
expect_fail observe-after-failure locked observe "$run_id" warm-3 "$p8_sha" PASS

# A second run proves the complete seven-observation acceptance path and
# catches component tampering before authorization.
prepare_output=$(locked prepare good)
run_id2=$(printf '%s\n' "$prepare_output" | sed -n \
	's/.*result=PASS action=prepare run_id=\([^ ]*\).*/\1/p')
run2=$workspace/reports/p8-runs/$run_id2
make_manifest "$run2"
chmod 0644 "$run2/artifact/Image"
cp -- "$run2/artifact/Image" "$scratch/Image.clean"
printf 'tamper\n' >>"$run2/artifact/Image"
expect_fail component-tamper locked flash-preflight "$run_id2"
cp -- "$scratch/Image.clean" "$run2/artifact/Image"
chmod 0444 "$run2/artifact/Image"
locked flash-preflight "$run_id2" >/dev/null
publish_flash_fixture "$run2"
locked flash-complete "$run_id2" >/dev/null
p8_sha2=$(value "$run2/BUILD-LOCK.env" p8_sha256)
for step in first-cold warm-1 warm-2 warm-3 cold-1 cold-2 cold-3; do
	locked observe "$run_id2" "$step" "$p8_sha2" PASS >/dev/null
done
[[ $(value "$run2/STATE.env" state) == ACCEPTED && \
	$(value "$run2/DEVICE-ACCEPTANCE.env" device_boot) == PASS ]] || \
	fail full-acceptance-state
"$cycle" status "$run_id2" | grep -q 'state=ACCEPTED' || fail accepted-status

[[ ! -e $sentinel ]] || fail device-command-was-called
printf 'P8_CYCLE_TEST result=PASS runs=2 negative_transitions=PASS no_clobber=PASS device_write=NONE p7_write=NONE\n'
