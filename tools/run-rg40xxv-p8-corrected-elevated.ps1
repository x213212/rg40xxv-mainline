$ErrorActionPreference = "Stop"
$base = "C:\Users\<your-user>\AppData\Local\Temp\rg40xxv-p8-corrected-v2"
$restore = Join-Path $base "restore.ps1"
$image = Join-Path $base "corrected-p8.img"
$log = Join-Path $base "restore3.log"
$result = Join-Path $base "elevated-result.txt"

try {
    & $restore -DriveLetter H -BackupPath $image -LogPath $log
    Set-Content -LiteralPath $result -Value "PASS"
    exit 0
}
catch {
    $detail = ($_ | Format-List * -Force | Out-String)
    Set-Content -LiteralPath $result -Value ("FAIL" + [Environment]::NewLine + $detail)
    exit 1
}
