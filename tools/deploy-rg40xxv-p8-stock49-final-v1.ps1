param(
    [string]$DriveLetter = "H",
    [string]$LogPath = "$env:TEMP\rg40xxv-p8-stock49-final-v1.log",
    [string]$PreWriteBackupPath = "$env:TEMP\rg40xxv-p8-before-stock49-final-v1.img"
)

$ErrorActionPreference = "Stop"

$deploy = Join-Path $PSScriptRoot "deploy-rg40xxv-p8-vblank-flush-v1.ps1"
$candidate = "\\wsl.localhost\Ubuntu\root\kernel\lab\candidates\rg40xxv-stock49-final-p8-v1\rg40xxv-stock49-final-p8-v1-persistent-legacy-p8.img"
$candidateSha256 = "0dda558be87a2f342eae31085f91d24c92a600e19baa1ac553ace12ccd92458c"

# stock49-final-v1: stock 4.9 handoff order (address-only takeover -> 3 real TCON
# vblanks -> full state -> Linux RCQ ownership), deferred IOMMU attach with cloned
# firmware PTE, TCON_TOP route preservation, GIC SPI 88 DE FINISH IRQ, one
# hardware-RCQ retry then fail-closed, and stock-aligned shutdown ordering.
# Status at package time: HOST_GATES_PASS_DEVICE_UNTESTED.
#
# Reuse the range-checked deployer. It requires an Administrator token, validates
# H: as the exact TF1 (size, GPT GUID, all eight partition offsets and sizes),
# verifies p4 recovery is byte-identical to stock, refuses to write unless the
# current p8 is one this workspace produced, saves the outgoing p8 to
# -PreWriteBackupPath, writes only the 64 MiB p8 range, then hashes the full
# readback and re-checks p4 before allowing the card to be ejected.
& $deploy `
    -DriveLetter $DriveLetter `
    -BackupPath $candidate `
    -ExpectedBackupSha256 $candidateSha256 `
    -LogPath $LogPath `
    -PreWriteBackupPath $PreWriteBackupPath
