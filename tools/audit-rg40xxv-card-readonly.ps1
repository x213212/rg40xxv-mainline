param(
    [int]$DiskNumber = 6,
    [string]$LogPath = "C:\Windows\Temp\rg40xxv-card-readonly-audit.log",
    [string]$P7ImagePath = "C:\Windows\Temp\rg40xxv-p7-readonly.img",
    [string]$GptHeadPath = "C:\Windows\Temp\rg40xxv-gpt-head-readonly.bin",
    [string]$GptTailPath = "C:\Windows\Temp\rg40xxv-gpt-tail-readonly.bin",
    [switch]$SkipP7
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ExpectedDiskBytes = [Int64]62516101120
$ExpectedDiskGuid = "ab6f3888-569a-4926-9668-80941dcb40bc"
$ExpectedSerial = "000000002964"
$ExpectedPartitionCount = 8
$P4Offset = [Int64]47332720640
$P4Bytes = [Int64]67108864
$P7Offset = [Int64]59210989568
$P7Bytes = [Int64]2679111680
$P8Offset = [Int64]61890101248
$P8Bytes = [Int64]67108864
$ExpectedP4Sha256 = "09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519"
$PinnedP8Sha256 = "a3e9774e3ac3e0db5771687ca53da9f0ae75a1808ca46d30cfdeb5bf91a772c3"
$BufferBytes = 4MB

function Write-Log([string]$Message) {
    $line = "{0:yyyy-MM-ddTHH:mm:ss.fffK} {1}" -f (Get-Date), $Message
    Add-Content -LiteralPath $LogPath -Value $line
    Write-Output $line
}

function Assert-Equal($Actual, $Expected, [string]$Name) {
    if ($Actual -ne $Expected) {
        throw "$Name mismatch: actual=$Actual expected=$Expected"
    }
}

function Get-RangeSha256(
    [System.IO.FileStream]$Stream,
    [Int64]$Offset,
    [Int64]$Length
) {
    $hash = [System.Security.Cryptography.IncrementalHash]::CreateHash(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256
    )
    try {
        [void]$Stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new($BufferBytes)
        $remaining = $Length
        while ($remaining -gt 0) {
            $wanted = [int][Math]::Min([Int64]$buffer.Length, $remaining)
            $read = $Stream.Read($buffer, 0, $wanted)
            if ($read -le 0) {
                throw "Unexpected EOF at raw offset $($Stream.Position)"
            }
            $hash.AppendData($buffer, 0, $read)
            $remaining -= $read
        }
        return ([BitConverter]::ToString($hash.GetHashAndReset()).Replace("-", "").ToLowerInvariant())
    }
    finally {
        $hash.Dispose()
    }
}

function Copy-RangeReadOnly(
    [System.IO.FileStream]$Source,
    [Int64]$Offset,
    [Int64]$Length,
    [string]$OutputPath
) {
    $target = [System.IO.FileStream]::new(
        $OutputPath,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None,
        $BufferBytes,
        [System.IO.FileOptions]::WriteThrough
    )
    try {
        [void]$Source.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new($BufferBytes)
        $remaining = $Length
        while ($remaining -gt 0) {
            $wanted = [int][Math]::Min([Int64]$buffer.Length, $remaining)
            $read = $Source.Read($buffer, 0, $wanted)
            if ($read -le 0) {
                throw "Unexpected EOF while copying p7 at raw offset $($Source.Position)"
            }
            $target.Write($buffer, 0, $read)
            $remaining -= $read
        }
        $target.Flush($true)
    }
    finally {
        $target.Dispose()
    }
}

Remove-Item -LiteralPath $LogPath -Force -ErrorAction SilentlyContinue
Write-Log "MODE=READ_ONLY no_card_writes=yes"

$disk = Get-Disk -Number $DiskNumber
Assert-Equal ([Int64]$disk.Size) $ExpectedDiskBytes "disk size"
Assert-Equal ($disk.SerialNumber.Trim()) $ExpectedSerial "disk serial"
Assert-Equal ($disk.PartitionStyle.ToString()) "GPT" "partition style"
Assert-Equal ($disk.Guid.ToString().Trim("{} ").ToLowerInvariant()) $ExpectedDiskGuid "disk GUID"
$partitions = @(Get-Partition -DiskNumber $DiskNumber | Sort-Object PartitionNumber)
Assert-Equal $partitions.Count $ExpectedPartitionCount "partition count"

$p4 = $partitions | Where-Object PartitionNumber -eq 4
$p7 = $partitions | Where-Object PartitionNumber -eq 7
$p8 = $partitions | Where-Object PartitionNumber -eq 8
Assert-Equal ([Int64]$p4.Offset) $P4Offset "p4 offset"
Assert-Equal ([Int64]$p4.Size) $P4Bytes "p4 size"
Assert-Equal ([Int64]$p7.Offset) $P7Offset "p7 offset"
Assert-Equal ([Int64]$p7.Size) $P7Bytes "p7 size"
Assert-Equal ([Int64]$p8.Offset) $P8Offset "p8 offset"
Assert-Equal ([Int64]$p8.Size) $P8Bytes "p8 size"
Write-Log "CARD_IDENTITY=PASS disk=$DiskNumber serial=$ExpectedSerial guid=$ExpectedDiskGuid"

$devicePath = "\\.\PhysicalDrive$DiskNumber"
$stream = [System.IO.FileStream]::new(
    $devicePath,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    [System.IO.FileShare]::ReadWrite,
    $BufferBytes,
    [System.IO.FileOptions]::SequentialScan
)
try {
    $p4Hash = Get-RangeSha256 $stream $P4Offset $P4Bytes
    $p8Hash = Get-RangeSha256 $stream $P8Offset $P8Bytes
    Write-Log "P4_SHA256=$p4Hash expected=$ExpectedP4Sha256 match=$($p4Hash -eq $ExpectedP4Sha256)"
    Write-Log "P8_SHA256=$p8Hash expected=$PinnedP8Sha256 match=$($p8Hash -eq $PinnedP8Sha256)"
    Copy-RangeReadOnly $stream 0 (34 * 512) $GptHeadPath
    Copy-RangeReadOnly $stream ($ExpectedDiskBytes - (34 * 512)) (34 * 512) $GptTailPath
    Write-Log "GPT_COPY=PASS head=$GptHeadPath tail=$GptTailPath"
    if (-not $SkipP7) {
        Copy-RangeReadOnly $stream $P7Offset $P7Bytes $P7ImagePath
    }
}
finally {
    $stream.Dispose()
}

if (-not $SkipP7) {
    $p7Hash = (Get-FileHash -LiteralPath $P7ImagePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Log "P7_COPY=PASS path=$P7ImagePath bytes=$P7Bytes sha256=$p7Hash"
}
else {
    Write-Log "P7_COPY=SKIPPED"
}
Write-Log "AUDIT=PASS"
