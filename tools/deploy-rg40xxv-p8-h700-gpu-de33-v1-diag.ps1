param(
    [string]$DriveLetter = "H",
    [string]$LogPath = "$env:TEMP\rg40xxv-p8-h700-gpu-de33-v1-diag.log",
    [string]$PreWriteBackupPath = "$env:TEMP\rg40xxv-p8-before-h700-gpu-de33-v1-diag.img"
)

$ErrorActionPreference = "Stop"

$deploy = Join-Path $PSScriptRoot "deploy-rg40xxv-p8-vblank-flush-v1.ps1"
$candidate = "\\wsl.localhost\Ubuntu\root\kernel\lab\candidates\rg40xxv-h700-gpu-de33-v1-diag\artifacts\rg40xxv-h700-gpu-de33-v1-diag-persistent-legacy-p8.img"
$candidateSha256 = "4920bd05512cccad9f23e1252c2f861d08d13f80584eab52cedb4adc732ea0ca"

# Reuse the range-checked deployer. It validates H: as the exact TF1,
# validates all eight GPT partition offsets, verifies p4 and current-p8
# provenance, preserves the outgoing p8, writes only p8, and hashes the full
# 64 MiB readback before allowing the card to be ejected.
& $deploy `
    -DriveLetter $DriveLetter `
    -BackupPath $candidate `
    -ExpectedBackupSha256 $candidateSha256 `
    -LogPath $LogPath `
    -PreWriteBackupPath $PreWriteBackupPath
