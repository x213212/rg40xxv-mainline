param(
    [string]$DriveLetter = "H",
    [string]$BackupPath = "\\wsl.localhost\Ubuntu\root\kernel\lab\candidates\rg40xxv-panel-adopt-closeflow-v2\rg40xxv-panel-adopt-closeflow-v2-persistent-legacy-p8.img",
    [string]$LogPath = "$env:TEMP\rg40xxv-p8-panel-adopt-closeflow-v2-deploy.log",
    [string]$PreWriteBackupPath = "$env:TEMP\rg40xxv-p8-before-panel-adopt-closeflow-v2.img",
    # Override together with -BackupPath to roll back to the pre-write image
    # (its SHA-256 is printed in the deploy log as PREWRITE_BACKUP_OK).
    [string]$ExpectedBackupSha256 = "46cb7e4eac97b599a6057a206e1e63379ec53d3e20eea811b7d61b6245a015ec"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$ExpectedDiskBytes = [Int64]62516101120
$ExpectedDiskGuid = "ab6f3888-569a-4926-9668-80941dcb40bc"
$ExpectedPartitionCount = 8
$P4Offset = [Int64]47332720640
$P4Bytes = [Int64]67108864
$P4Sha256 = "09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519"
$P8Offset = [Int64]61890101248
$P8Bytes = [Int64]67108864
# Every p8 image this workspace has ever written to TF1.  The card must
# currently hold one of these, otherwise the script refuses to write.
$KnownCurrentP8Sha256 = @(
    "09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519", # exact stock p4 image used as direct-stock p8
    "9bffb5f176a377597ac402cc364a344bda2c51a371bd6ec9b430f9de86ca6101", # 9b pageflip-nopulse-v1 (tearing, visible)
    "a3e9774e3ac3e0db5771687ca53da9f0ae75a1808ca46d30cfdeb5bf91a772c3", # a3 stock-panel-openfix-rdmafixed-v2
    "f96a3cccd4f5161248b13c05c91f5841724192ee617c903ec2e23805d91ab61f", # v3 noelide-log (black selector)
    "963095dafc9ef35759566d3f0be4ddcbcbf8dcf70ae456215290989dfd5d76a0", # noelide-v2
    "bc6a49010245d40c8fa20a661cb365734e4ca07e9471013fa92d4ecbb3dce15c", # nodefer-btlate-v4
    "a6078bf8eda1a25fc1194064eb68b5200d43b4fe44b8abc76f8e73247299784a", # warm-reboot-de33-corrected-v3
    "c170b30328298c3416f8db11468f877415f67fea14e0ae68ebe997e2ef1fe022", # warm-reboot-de33-corrected-v2
    "6b8148e589fd033022251ddf5d2aaadc295c67d056d39ca415ffbb0ffadca8a6", # warm-reboot-de33-v1
    "01541ca8f85e8c94f6b12ee104a9e14f243a2a6ae63d9a9c08320d1f8d8b99fc", # stock-panel-warmfix-v1
    "c04842ea5d400df80befe166cd7783617fb5f92b1597c52910e5d10c284c8fb3", # stock-panel-openfix-v1
    "abcce1ae92feda19a15644c33680ef736498aacc088c656557128af11ff25fcb", # production-v1 display-no-rcq golden
    "360a1d833f99276e1f4bca4051fd1fc505324f94d2a775ef6638c850d7f22891", # de33-two-phase-tcon-v1
    "70e5308327450696a08b12cf82d7d31f64f89e8b020ceae252f88879f7a30d4f", # de33-two-phase-v1
    "e53c76e5c2b82320fc686d7d45f7d007252b09921f84fd2aa0b2c1227806a118", # quiesce-tcon-v1
    "26aeddf0aa205ca30b4fa621cc919cdf4b910edabd51cf3fa687fad0b87af136", # de33-vblank-flush-v1
    "4920bd05512cccad9f23e1252c2f861d08d13f80584eab52cedb4adc732ea0ca", # h700-gpu-de33-v1-diag (device FAIL: corrupted display timing)
    "0dda558be87a2f342eae31085f91d24c92a600e19baa1ac553ace12ccd92458c", # stock49-final-p8-v1 (HOST_GATES_PASS_DEVICE_UNTESTED)
    "26aeddf0aa205ca30b4fa621cc919cdf4b910edabd51cf3fa687fad0b87af136", # de33-vblank-flush-v1 (no tearing, visible)
    "3e37b2f3f7287dbb0cfe8526ac73fb4ea063a5adec89860966f2472160ed45bc", # selector 10/10 visible baseline
    "b441d9a8cc58282c01f2ae696b547c46baf760a60327fe4384f9eae96249f5d1", # mux fix + panel force cycle (black)
    "b43a7821d09a703085106c872354d3e4f03c76c9b473364c43f8ec02afc056e2", # forced PIO + IOMMU
    "125276a82b51eb68e745c3761e801704487384eb4761052b4fda12ecf7135202", # no zero-gap cycle (2nd boot visible)
    "b7e6ca80d5b8917100d6b815560daf407d15b7354d1bf9d622a8e0df4a742222", # raw bit-bang transport
    "da3fc9b6cf3953440e4aa88916ad775b0ed81b711b6388d7c090b2ba39f18a3f", # iommu-active-bypass-v1
    "64c4a744a7ddbbe969d954e09c5e9ede6121370104294d3d4912e13175742425", # hwrcq-v2 (last visible hwrcq)
    "67bd80dce92b8550b4fce88ee22f1bce0384ad9491c007b10ec6176234838a34", # softcold-hwrcq-v3 FAIL
    "7db54caa8ad76b1b10a107298fe3a539340c8d7be699bb201484dda4742a8bcc", # softcold-hwrcq-coldpath-v4 FAIL
    "3a5bb49e524265dcc26cca36fb7435d4560537cc3da6bfbf3f675f2067a73c20", # hwrcq-resilient-v5 FAIL
    "09bb1eee75a3ae1b2950c0886c0777abd1a48a266e045376a1b9e098894fc519", # stock p8 (== p4 recovery)
    "f6ce63c82a5bb96140571d309022b7b54cdc6b7db192137c3cf8c4508444094d" # full-coldinit-v7 FAIL (boot loop, on card now)
)
$BackupP8Sha256 = $ExpectedBackupSha256.ToLowerInvariant()
$BufferBytes = 4MB
$ExpectedPartitionLayout = @(
    [pscustomobject]@{ Number = 1; Offset = [Int64]37748736;    Size = [Int64]47244640256; Drive = "H" }
    [pscustomobject]@{ Number = 2; Offset = [Int64]47282388992; Size = [Int64]33554432;    Drive = "" }
    [pscustomobject]@{ Number = 3; Offset = [Int64]47315943424; Size = [Int64]16777216;    Drive = "" }
    [pscustomobject]@{ Number = 4; Offset = [Int64]47332720640; Size = [Int64]67108864;    Drive = "" }
    [pscustomobject]@{ Number = 5; Offset = [Int64]47399829504; Size = [Int64]7516192768;  Drive = "" }
    [pscustomobject]@{ Number = 6; Offset = [Int64]54916022272; Size = [Int64]4294967296;  Drive = "" }
    [pscustomobject]@{ Number = 7; Offset = [Int64]59210989568; Size = [Int64]2679111680;  Drive = "" }
    [pscustomobject]@{ Number = 8; Offset = [Int64]61890101248; Size = [Int64]67108864;    Drive = "" }
)

function Write-Log([string]$Message) {
    $line = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $Message
    $line | Tee-Object -FilePath $LogPath -Append
}

function Assert-Equal($Actual, $Expected, [string]$Name) {
    if ($Actual -ne $Expected) {
        throw "$Name mismatch: actual=$Actual expected=$Expected"
    }
}

function Get-RangeSha256([string]$Path, [Int64]$Offset, [Int64]$Length) {
    $stream = [System.IO.FileStream]::new(
        $Path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite,
        $BufferBytes,
        [System.IO.FileOptions]::SequentialScan
    )
    $hash = [System.Security.Cryptography.IncrementalHash]::CreateHash(
        [System.Security.Cryptography.HashAlgorithmName]::SHA256
    )
    try {
        [void]$stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new($BufferBytes)
        $remaining = $Length
        while ($remaining -gt 0) {
            $wanted = [int][Math]::Min([Int64]$buffer.Length, $remaining)
            $read = $stream.Read($buffer, 0, $wanted)
            if ($read -le 0) {
                throw "Unexpected EOF while hashing $Path at offset $($stream.Position)"
            }
            $hash.AppendData($buffer, 0, $read)
            $remaining -= $read
        }
        return ([BitConverter]::ToString($hash.GetHashAndReset()).Replace("-", "").ToLowerInvariant())
    }
    finally {
        $hash.Dispose()
        $stream.Dispose()
    }
}

function Read-RangeToFile([string]$DevicePath, [Int64]$Offset, [Int64]$Length, [string]$OutPath) {
    $source = [System.IO.FileStream]::new(
        $DevicePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite,
        $BufferBytes,
        [System.IO.FileOptions]::SequentialScan
    )
    $target = [System.IO.FileStream]::new(
        $OutPath,
        [System.IO.FileMode]::Create,
        [System.IO.FileAccess]::Write,
        [System.IO.FileShare]::None,
        $BufferBytes,
        [System.IO.FileOptions]::WriteThrough
    )
    try {
        [void]$source.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new($BufferBytes)
        $remaining = $Length
        while ($remaining -gt 0) {
            $wanted = [int][Math]::Min([Int64]$buffer.Length, $remaining)
            $read = $source.Read($buffer, 0, $wanted)
            if ($read -le 0) {
                throw "Unexpected EOF while backing up p8 at offset $($source.Position)"
            }
            $target.Write($buffer, 0, $read)
            $remaining -= $read
        }
        $target.Flush($true)
    }
    finally {
        $target.Dispose()
        $source.Dispose()
    }
}

function Write-ImageRange([string]$ImagePath, [string]$DevicePath, [Int64]$Offset, [Int64]$Length) {
    $source = [System.IO.FileStream]::new(
        $ImagePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::Read,
        $BufferBytes,
        [System.IO.FileOptions]::SequentialScan
    )
    $target = [System.IO.FileStream]::new(
        $DevicePath,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::ReadWrite,
        [System.IO.FileShare]::ReadWrite,
        $BufferBytes,
        [System.IO.FileOptions]::WriteThrough
    )
    try {
        [void]$target.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $buffer = [byte[]]::new($BufferBytes)
        $remaining = $Length
        while ($remaining -gt 0) {
            $wanted = [int][Math]::Min([Int64]$buffer.Length, $remaining)
            $read = $source.Read($buffer, 0, $wanted)
            if ($read -le 0) {
                throw "Unexpected EOF in backup image"
            }
            $target.Write($buffer, 0, $read)
            $remaining -= $read
        }
        if ($source.Position -ne $Length) {
            throw "Backup image length changed while writing"
        }
        $target.Flush($true)
    }
    finally {
        $target.Dispose()
        $source.Dispose()
    }
}

Set-Content -LiteralPath $LogPath -Value "RG40XX-V p8 deploy panel-adopt-closeflow-v2"

$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Administrator token is required"
}

Write-Log "PRECHECK drive=$($DriveLetter): backup=$BackupPath"
$backup = Get-Item -LiteralPath $BackupPath
Assert-Equal ([Int64]$backup.Length) $P8Bytes "backup size"
$backupHash = (Get-FileHash -LiteralPath $BackupPath -Algorithm SHA256).Hash.ToLowerInvariant()
Assert-Equal $backupHash $BackupP8Sha256 "backup SHA256"
Write-Log "BACKUP_OK bytes=$($backup.Length) sha256=$backupHash"

$partition = Get-Partition -DriveLetter $DriveLetter
$disk = $partition | Get-Disk
$partitions = @(Get-Partition -DiskNumber $disk.Number | Sort-Object PartitionNumber)

Assert-Equal ([Int64]$disk.Size) $ExpectedDiskBytes "disk size"
Assert-Equal $disk.PartitionStyle.ToString() "GPT" "partition style"
Assert-Equal $disk.BusType.ToString() "USB" "bus type"
Assert-Equal $disk.Guid.ToString().Trim("{}") $ExpectedDiskGuid "GPT disk GUID"
Assert-Equal $partitions.Count $ExpectedPartitionCount "partition count"
Assert-Equal $partition.PartitionNumber 1 "H partition number"

foreach ($expected in $ExpectedPartitionLayout) {
    $actual = $partitions | Where-Object PartitionNumber -eq $expected.Number
    if ($null -eq $actual) {
        throw "p$($expected.Number) is missing"
    }
    Assert-Equal ([Int64]$actual.Offset) ([Int64]$expected.Offset) "p$($expected.Number) offset"
    Assert-Equal ([Int64]$actual.Size) ([Int64]$expected.Size) "p$($expected.Number) size"
    $actualDrive = if (
        $null -eq $actual.DriveLetter -or
        [string]::IsNullOrWhiteSpace([string]$actual.DriveLetter)
    ) {
        ""
    }
    else {
        ([string]$actual.DriveLetter).ToUpperInvariant()
    }
    Assert-Equal $actualDrive $expected.Drive "p$($expected.Number) drive letter"
}
Write-Log "LAYOUT_OK p1-p8 offsets_sizes_and_drive_letters=EXACT"

$p4 = $partitions | Where-Object PartitionNumber -eq 4
$p8 = $partitions | Where-Object PartitionNumber -eq 8
Assert-Equal ([Int64]$p4.Offset) $P4Offset "p4 offset"
Assert-Equal ([Int64]$p4.Size) $P4Bytes "p4 size"
Assert-Equal ([Int64]$p8.Offset) $P8Offset "p8 offset"
Assert-Equal ([Int64]$p8.Size) $P8Bytes "p8 size"

$diskNumber = $disk.Number
$devicePath = "\\.\PhysicalDrive$diskNumber"
Write-Log "CARD_OK disk=$diskNumber bytes=$($disk.Size) guid=$($disk.Guid) p8_offset=$P8Offset p8_bytes=$P8Bytes"

$wasOffline = [bool]$disk.IsOffline
$madeOffline = $false
$removedAccessPath = $false
$accessPath = "$($DriveLetter):\"
try {
    if (-not $wasOffline) {
        if ($disk.BusType.ToString() -eq "USB") {
            Write-Log "Dismounting only $accessPath for removable-media raw access"
            Remove-PartitionAccessPath -DiskNumber $diskNumber -PartitionNumber $partition.PartitionNumber -AccessPath $accessPath
            $removedAccessPath = $true
        }
        else {
            Write-Log "Taking only PhysicalDrive$diskNumber offline for exclusive raw access"
            Set-Disk -Number $diskNumber -IsOffline $true
            $madeOffline = $true
        }
    }

    $p4Hash = Get-RangeSha256 $devicePath $P4Offset $P4Bytes
    Assert-Equal $p4Hash $P4Sha256 "p4 SHA256"
    Write-Log "P4_OK sha256=$p4Hash"

    $currentP8Hash = Get-RangeSha256 $devicePath $P8Offset $P8Bytes
    Write-Log "CURRENT_P8 sha256=$currentP8Hash"
    if ($currentP8Hash -eq $BackupP8Sha256) {
        Write-Log "DEPLOY_ALREADY_COMPLETE"
    }
    elseif ($KnownCurrentP8Sha256 -contains $currentP8Hash) {
        # Keep a full copy of the outgoing p8 so this write is reversible.
        Read-RangeToFile $devicePath $P8Offset $P8Bytes $PreWriteBackupPath
        $preHash = (Get-FileHash -LiteralPath $PreWriteBackupPath -Algorithm SHA256).Hash.ToLowerInvariant()
        Assert-Equal $preHash $currentP8Hash "pre-write backup SHA256"
        Write-Log "PREWRITE_BACKUP_OK path=$PreWriteBackupPath sha256=$preHash"

        Write-Log "Writing exactly 64 MiB to p8; no other byte range is targeted"
        Write-ImageRange $BackupPath $devicePath $P8Offset $P8Bytes
        $readbackHash = Get-RangeSha256 $devicePath $P8Offset $P8Bytes
        Assert-Equal $readbackHash $BackupP8Sha256 "p8 readback SHA256"
        $p4AfterHash = Get-RangeSha256 $devicePath $P4Offset $P4Bytes
        Assert-Equal $p4AfterHash $P4Sha256 "p4 post-write SHA256"
        Write-Log "DEPLOY_PASS p8_sha256=$readbackHash previous=$currentP8Hash p4=UNCHANGED"
    }
    else {
        throw "Current p8 is not one of the images this workspace wrote; refusing write (sha256=$currentP8Hash)"
    }
}
finally {
    if ($madeOffline) {
        Set-Disk -Number $diskNumber -IsOffline $false
        Write-Log "PhysicalDrive$diskNumber returned online"
    }
    if ($removedAccessPath) {
        Add-PartitionAccessPath -DiskNumber $diskNumber -PartitionNumber $partition.PartitionNumber -AccessPath $accessPath
        Write-Log "$accessPath mounted again"
    }
}

Write-Log "DONE_SAFE_TO_EJECT"
