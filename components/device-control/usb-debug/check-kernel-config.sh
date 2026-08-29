#!/usr/bin/env bash
set -euo pipefail

usage() {
    printf '用法：%s [核心 config 檔]\n' "$0" >&2
}

if (($# > 1)); then
    usage
    exit 2
fi

tmp_config=
cleanup() {
    if [[ -n "$tmp_config" ]]; then
        rm -f -- "$tmp_config"
    fi
}
trap cleanup EXIT

if (($# == 1)); then
    config=$1
    if [[ ! -f "$config" || -L "$config" ]]; then
        printf '錯誤：config 必須是一般檔案：%s\n' "$config" >&2
        exit 2
    fi
elif [[ -r /proc/config.gz ]]; then
    tmp_config=$(mktemp /tmp/rg40xxv-kconfig.XXXXXX)
    gzip -dc /proc/config.gz >"$tmp_config"
    config=$tmp_config
elif [[ -r "/boot/config-$(uname -r)" ]]; then
    config="/boot/config-$(uname -r)"
else
    printf '錯誤：找不到 /proc/config.gz 或目前核心的 /boot/config-*\n' >&2
    exit 2
fi

required=(
    CONFIG_CONFIGFS_FS
    CONFIG_USB_GADGET
    CONFIG_USB_LIBCOMPOSITE
    CONFIG_USB_CONFIGFS
    CONFIG_USB_CONFIGFS_ACM
    CONFIG_USB_CONFIGFS_RNDIS
    CONFIG_USB_U_ETHER
    CONFIG_USB_F_ACM
    CONFIG_TTY
)

missing=0
for symbol in "${required[@]}"; do
    if grep -Eq "^${symbol}=(y|m)$" "$config"; then
        printf 'OK      %s\n' "$symbol"
    else
        printf 'MISSING %s\n' "$symbol"
        missing=1
    fi
done

if grep -Eq '^CONFIG_(USB_.+_UDC|USB_MUSB_HDRC|USB_DWC2)=(y|m)$' "$config"; then
    printf 'OK      找到候選 UDC controller driver\n'
else
    printf 'WARNING 無法由 config 找到常見 UDC driver；仍須以 DTS 與 /sys/class/udc 驗證\n' >&2
fi

if ((missing)); then
    printf '結果：USB debug 核心必要條件不完整。\n' >&2
    exit 1
fi

printf '結果：通用 configfs/ACM/RNDIS 條件齊全；仍須實機驗證 UDC 與 Type-C data role。\n'

