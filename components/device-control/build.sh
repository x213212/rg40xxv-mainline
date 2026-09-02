#!/usr/bin/env bash
set -euo pipefail

DEVICE_CONTROL_ROOT=$(cd -- "$(dirname -- "$0")" && pwd)
DEVICE_CONTROL_BUILD="$DEVICE_CONTROL_ROOT/build"
ACTION=${1:-check}
SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-1787500800}

[[ $SOURCE_DATE_EPOCH =~ ^[0-9]+$ ]] || {
    printf 'SOURCE_DATE_EPOCH 必須是非負整數\n' >&2
    exit 2
}

SCRIPTS=(
    "$DEVICE_CONTROL_ROOT/build.sh"
    "$DEVICE_CONTROL_ROOT/usb-debug/usb-debugctl"
    "$DEVICE_CONTROL_ROOT/usb-debug/check-kernel-config.sh"
    "$DEVICE_CONTROL_ROOT/debug-logctl/debug-logctl"
    "$DEVICE_CONTROL_ROOT/save-guard/save-guard"
    "$DEVICE_CONTROL_ROOT/power-lock/power-lockctl"
    "$DEVICE_CONTROL_ROOT/ui-hardwarectl/ui-hardwarectl"
    "$DEVICE_CONTROL_ROOT/cpu-policy/cpu-policyctl"
    "$DEVICE_CONTROL_ROOT/vpn/vpn-profilectl"
    "$DEVICE_CONTROL_ROOT/vpn/vpn-firewall"
	"$DEVICE_CONTROL_ROOT/network/rg40xxv-network-control"
	"$DEVICE_CONTROL_ROOT/power-monitor/rg40xxv-power-monitor"
    "$DEVICE_CONTROL_ROOT/tests/test-usb-debug.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-debug-logctl.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-save-guard.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-power-lock.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-ui-hardwarectl.sh"
	"$DEVICE_CONTROL_ROOT/tests/test-volume.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-cpu-policy.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-vpn.sh"
	"$DEVICE_CONTROL_ROOT/tests/test-network-control.sh"
	"$DEVICE_CONTROL_ROOT/tests/test-power-monitor.sh"
)

C_SOURCES=(
	"$DEVICE_CONTROL_ROOT/volume/rg40xxv-volume-daemon.c"
	"$DEVICE_CONTROL_ROOT/volume/rg40xxv-volume-ctl.c"
)

run_checks() {
	local host_cc=${DEVICE_CONTROL_HOST_CC:-gcc}

    bash -n "${SCRIPTS[@]}"
	command -v "$host_cc" >/dev/null 2>&1 || {
		printf '缺少 host C compiler：%s\n' "$host_cc" >&2
		exit 1
	}
	"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
		-fsyntax-only "${C_SOURCES[@]}"
	"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic \
		-DRG40XXV_VOLUME_TESTING=1 -fsyntax-only "${C_SOURCES[@]}"
    if command -v shellcheck >/dev/null 2>&1; then
        shellcheck "${SCRIPTS[@]}"
    else
        printf 'warning: shellcheck 未安裝，已略過\n' >&2
    fi
}

run_tests() {
    "$DEVICE_CONTROL_ROOT/tests/test-usb-debug.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-debug-logctl.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-save-guard.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-power-lock.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-ui-hardwarectl.sh"
	"$DEVICE_CONTROL_ROOT/tests/test-volume.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-cpu-policy.sh"
    "$DEVICE_CONTROL_ROOT/tests/test-vpn.sh"
	"$DEVICE_CONTROL_ROOT/tests/test-network-control.sh"
	"$DEVICE_CONTROL_ROOT/tests/test-power-monitor.sh"
}

build_package() {
	local stage archive target_cc=${DEVICE_CONTROL_CC:-aarch64-linux-gnu-gcc-12}

	for tool in gzip tar "$target_cc"; do
        command -v "$tool" >/dev/null 2>&1 || {
            printf '缺少套件工具：%s\n' "$tool" >&2
            exit 1
        }
    done

    stage="$DEVICE_CONTROL_BUILD/stage"
    archive="$DEVICE_CONTROL_BUILD/rg40xxv-device-control.tar.gz"
    rm -rf -- "$stage"
    mkdir -p \
        "$stage/usr/libexec/rg40xxv" \
        "$stage/usr/sbin" \
        "$stage/usr/lib/systemd/system" \
		"$stage/usr/lib/tmpfiles.d" \
        "$stage/usr/lib/systemd/network" \
		"$stage/etc/NetworkManager/conf.d" \
		"$stage/etc/systemd/system/multi-user.target.d" \
		"$stage/etc/systemd/system/rg40xxv-time-sync.service.d" \
		"$stage/etc/systemd/system/rg40xxv-ui.service.d" \
        "$stage/etc/default" \
        "$stage/usr/share/doc/rg40xxv-device-control"
	mkdir -p "$DEVICE_CONTROL_BUILD/bin"
	"$target_cc" -std=c11 -Os -Wall -Wextra -Werror -Wpedantic \
		-fPIE -pie -Wl,-z,relro,-z,now \
		"$DEVICE_CONTROL_ROOT/volume/rg40xxv-volume-daemon.c" \
		-o "$DEVICE_CONTROL_BUILD/bin/rg40xxv-volume-daemon"
	"$target_cc" -std=c11 -Os -Wall -Wextra -Werror -Wpedantic \
		-fPIE -pie -Wl,-z,relro,-z,now \
		"$DEVICE_CONTROL_ROOT/volume/rg40xxv-volume-ctl.c" \
		-o "$DEVICE_CONTROL_BUILD/bin/rg40xxv-volume-ctl"
	install -m 0755 "$DEVICE_CONTROL_BUILD/bin/rg40xxv-volume-daemon" \
		"$stage/usr/libexec/rg40xxv/rg40xxv-volume-daemon"
	install -m 0755 "$DEVICE_CONTROL_BUILD/bin/rg40xxv-volume-ctl" \
		"$stage/usr/libexec/rg40xxv/rg40xxv-volume-ctl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/usb-debug/usb-debugctl" \
        "$stage/usr/libexec/rg40xxv/usb-debugctl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/usb-debug/check-kernel-config.sh" \
        "$stage/usr/libexec/rg40xxv/check-usb-debug-kernel-config"
    install -m 0755 "$DEVICE_CONTROL_ROOT/debug-logctl/debug-logctl" \
        "$stage/usr/sbin/debug-logctl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/save-guard/save-guard" \
        "$stage/usr/sbin/save-guard"
    install -m 0755 "$DEVICE_CONTROL_ROOT/power-lock/power-lockctl" \
        "$stage/usr/sbin/power-lockctl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/ui-hardwarectl/ui-hardwarectl" \
        "$stage/usr/sbin/ui-hardwarectl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/cpu-policy/cpu-policyctl" \
        "$stage/usr/sbin/cpu-policyctl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/vpn/vpn-profilectl" \
        "$stage/usr/sbin/vpn-profilectl"
    install -m 0755 "$DEVICE_CONTROL_ROOT/vpn/vpn-firewall" \
        "$stage/usr/sbin/vpn-firewall"
	install -m 0755 "$DEVICE_CONTROL_ROOT/network/rg40xxv-network-control" \
		"$stage/usr/sbin/rg40xxv-network-control"
	install -m 0755 "$DEVICE_CONTROL_ROOT/power-monitor/rg40xxv-power-monitor" \
		"$stage/usr/sbin/rg40xxv-power-monitor"
    install -m 0644 "$DEVICE_CONTROL_ROOT/usb-debug/rg40xxv-usb-debug.service" \
        "$stage/usr/lib/systemd/system/"
    install -m 0644 "$DEVICE_CONTROL_ROOT/usb-debug/rg40xxv-usb-debug-getty.service" \
        "$stage/usr/lib/systemd/system/"
    install -m 0644 "$DEVICE_CONTROL_ROOT/usb-debug/99-rg40xxv-usb-debug.network" \
        "$stage/usr/lib/systemd/network/"
    install -m 0644 "$DEVICE_CONTROL_ROOT/usb-debug/usb-debug.default" \
        "$stage/etc/default/rg40xxv-usb-debug"
    mkdir -p "$stage/etc/systemd/logind.conf.d"
    install -m 0644 "$DEVICE_CONTROL_ROOT/power-lock/60-rg40xxv-power-key.conf" \
        "$stage/etc/systemd/logind.conf.d/"
    install -m 0644 "$DEVICE_CONTROL_ROOT/cpu-policy/rg40xxv-cpu-ui-policy.service" \
        "$stage/usr/lib/systemd/system/"
	install -m 0644 "$DEVICE_CONTROL_ROOT/volume/rg40xxv-volume.service" \
		"$stage/usr/lib/systemd/system/"
	install -m 0644 "$DEVICE_CONTROL_ROOT/volume/rg40xxv-volume.conf" \
		"$stage/usr/lib/tmpfiles.d/"
    install -m 0644 "$DEVICE_CONTROL_ROOT/vpn/openvpn@.service" \
        "$stage/usr/lib/systemd/system/"
	install -m 0644 "$DEVICE_CONTROL_ROOT/network/rg40xxv-network-prepare.service" \
		"$stage/usr/lib/systemd/system/"
	install -m 0644 "$DEVICE_CONTROL_ROOT/network/default-wifi-powersave-on.conf" \
		"$stage/etc/NetworkManager/conf.d/default-wifi-powersave-on.conf"
	install -m 0644 "$DEVICE_CONTROL_ROOT/network/85-rg40xxv-network.conf" \
		"$stage/etc/systemd/system/multi-user.target.d/85-rg40xxv-network.conf"
	install -m 0644 "$DEVICE_CONTROL_ROOT/network/20-network-ready.conf" \
		"$stage/etc/systemd/system/rg40xxv-time-sync.service.d/20-network-ready.conf"
	install -m 0644 "$DEVICE_CONTROL_ROOT/network/20-network-ready-ui.conf" \
		"$stage/etc/systemd/system/rg40xxv-ui.service.d/20-network-ready.conf"
    install -m 0644 "$DEVICE_CONTROL_ROOT/README-zh-TW.md" \
        "$stage/usr/share/doc/rg40xxv-device-control/README-zh-TW.md"
    tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 \
        --numeric-owner --format=gnu -C "$stage" -cf - . | \
        gzip -n -9 >"$archive.tmp"
    mv -f -- "$archive.tmp" "$archive"
    printf '候選套件：%s\n' "$archive"
    printf '注意：未安裝、未啟用、未寫入實機。\n'
}

case "$ACTION" in
    check)
        run_checks
        ;;
    test)
        run_checks
        run_tests
        ;;
    package)
        run_checks
        run_tests
        build_package
        ;;
    *)
        printf '用法：%s [check|test|package]\n' "$0" >&2
        exit 2
        ;;
esac
