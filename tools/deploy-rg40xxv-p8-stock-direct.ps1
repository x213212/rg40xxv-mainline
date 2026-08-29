param(
    [string]$DriveLetter = "H",
    [string]$LogPath = "$env:TEMP\rg40xxv-p8-stock-direct.log",
    [string]$PreWriteBackupPath = "$env:TEMP\rg40xxv-p8-before-stock-direct.img"
)

$ErrorActionPreference = "Stop"

$deploy = Join-Path $PSScriptRoot "deploy-rg40xxv-p8-vblank-flush-v1.ps1"
$stock = "\\wsl.localhost\Ubuntu\root\kernel\lab\boot-stock-20260824\capture\boot-p4.img"
$stockSha256 = "09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519"

# Reuse the range-checked deployer. It validates the exact TF1 identity,
# all eight partition offsets, the untouched p4 hash, current p8 provenance,
# writes only the 64 MiB p8 range, and verifies the full readback.
& $deploy `
    -DriveLetter $DriveLetter `
    -BackupPath $stock `
    -ExpectedBackupSha256 $stockSha256 `
    -LogPath $LogPath `
    -PreWriteBackupPath $PreWriteBackupPath
