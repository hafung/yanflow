param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$ApplicationIcon,
    [string]$FloatingIcon = "",
    [string]$ListeningIcon = ""
)

$ErrorActionPreference = "Stop"

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class NativeResourceUpdate
{
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr BeginUpdateResource(string fileName, bool deleteExistingResources);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool UpdateResource(IntPtr update, IntPtr type, IntPtr name,
        ushort language, byte[] data, uint dataSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool EndUpdateResource(IntPtr update, bool discard);
}
"@

function Throw-LastWin32Error([string]$Operation) {
    $code = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    throw [ComponentModel.Win32Exception]::new($code, $Operation)
}

function Set-IcoResource([IntPtr]$Update, [string]$Path, [int]$GroupId, [int]$FirstImageId) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $reserved = $reader.ReadUInt16()
        $type = $reader.ReadUInt16()
        $count = $reader.ReadUInt16()
        if ($reserved -ne 0 -or $type -ne 1 -or $count -lt 1) {
            throw "Invalid ICO file: $Path"
        }

        $entries = @()
        for ($index = 0; $index -lt $count; $index++) {
            $entries += [pscustomobject]@{
                Width = $reader.ReadByte()
                Height = $reader.ReadByte()
                Colors = $reader.ReadByte()
                Reserved = $reader.ReadByte()
                Planes = $reader.ReadUInt16()
                Bits = $reader.ReadUInt16()
                Size = $reader.ReadUInt32()
                Offset = $reader.ReadUInt32()
                Id = $FirstImageId + $index
            }
        }

        foreach ($entry in $entries) {
            $stream.Position = $entry.Offset
            $data = $reader.ReadBytes([int]$entry.Size)
            if ($data.Length -ne $entry.Size) { throw "Truncated ICO image: $Path" }
            $ok = [NativeResourceUpdate]::UpdateResource($Update, [IntPtr]3,
                [IntPtr]$entry.Id, [uint16]0, $data, [uint32]$data.Length)
            if (-not $ok) { Throw-LastWin32Error "Update RT_ICON $($entry.Id)" }
        }

        $groupStream = [IO.MemoryStream]::new()
        $groupWriter = [IO.BinaryWriter]::new($groupStream)
        try {
            $groupWriter.Write([uint16]0)
            $groupWriter.Write([uint16]1)
            $groupWriter.Write([uint16]$entries.Count)
            foreach ($entry in $entries) {
                $groupWriter.Write([byte]$entry.Width)
                $groupWriter.Write([byte]$entry.Height)
                $groupWriter.Write([byte]$entry.Colors)
                $groupWriter.Write([byte]$entry.Reserved)
                $groupWriter.Write([uint16]$entry.Planes)
                $groupWriter.Write([uint16]$entry.Bits)
                $groupWriter.Write([uint32]$entry.Size)
                $groupWriter.Write([uint16]$entry.Id)
            }
            $groupWriter.Flush()
            $group = $groupStream.ToArray()
        } finally {
            $groupWriter.Dispose()
            $groupStream.Dispose()
        }
        $ok = [NativeResourceUpdate]::UpdateResource($Update, [IntPtr]14,
            [IntPtr]$GroupId, [uint16]0, $group, [uint32]$group.Length)
        if (-not $ok) { Throw-LastWin32Error "Update RT_GROUP_ICON $GroupId" }
        return $FirstImageId + $count
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$Executable = [IO.Path]::GetFullPath($Executable)
$ApplicationIcon = [IO.Path]::GetFullPath($ApplicationIcon)
if (-not (Test-Path $Executable)) { throw "Executable not found: $Executable" }
if (-not (Test-Path $ApplicationIcon)) { throw "Application icon not found: $ApplicationIcon" }
if (-not [string]::IsNullOrWhiteSpace($FloatingIcon)) {
    $FloatingIcon = [IO.Path]::GetFullPath($FloatingIcon)
    if (-not (Test-Path $FloatingIcon)) { throw "Floating icon not found: $FloatingIcon" }
}
if (-not [string]::IsNullOrWhiteSpace($ListeningIcon)) {
    $ListeningIcon = [IO.Path]::GetFullPath($ListeningIcon)
    if (-not (Test-Path $ListeningIcon)) { throw "Listening icon not found: $ListeningIcon" }
}

$update = [NativeResourceUpdate]::BeginUpdateResource($Executable, $false)
if ($update -eq [IntPtr]::Zero) { Throw-LastWin32Error "BeginUpdateResource" }
$committed = $false
try {
    $nextImageId = Set-IcoResource $update $ApplicationIcon 1 1
    if (-not [string]::IsNullOrWhiteSpace($FloatingIcon)) {
        [void](Set-IcoResource $update $FloatingIcon 2 ([math]::Max(101, $nextImageId)))
    }
    if (-not [string]::IsNullOrWhiteSpace($ListeningIcon)) {
        [void](Set-IcoResource $update $ListeningIcon 3 201)
    }
    if (-not [NativeResourceUpdate]::EndUpdateResource($update, $false)) {
        Throw-LastWin32Error "EndUpdateResource"
    }
    $committed = $true
} finally {
    if (-not $committed) { [void][NativeResourceUpdate]::EndUpdateResource($update, $true) }
}

Write-Host "Embedded executable icon resources in $Executable"
