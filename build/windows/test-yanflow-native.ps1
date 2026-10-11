$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$artifacts = Join-Path $repoRoot "build\artifacts"
$package = Join-Path $artifacts "yanflow-windows-x64"
$archive = Join-Path $artifacts "YanFlow-native-windows-x64.zip"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$dumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source

$executables = @("yanflow.exe", "funasr\yanflow-asr-worker.exe", "csc\yanflow-csc-worker.exe")
foreach ($relativePath in $executables) {
    $executable = Join-Path $package $relativePath
    $headers = (& $dumpbin /headers $executable) -join "`n"
    if ($LASTEXITCODE -ne 0 -or $headers -notmatch "8664 machine") {
        throw "Not a Windows x64 executable: $relativePath"
    }
    if ($relativePath -eq "yanflow.exe" -and $headers -notmatch "2 subsystem \(Windows GUI\)") {
        throw "YanFlow must use the Windows GUI subsystem"
    }
    $dependencies = (& $dumpbin /dependents $executable) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect dependencies: $relativePath" }
    $imports = @([regex]::Matches($dependencies, "(?im)^\s+([a-z0-9_.-]+\.dll)\s*$") |
        ForEach-Object { $_.Groups[1].Value })
    if ($imports.Count -eq 0) { throw "No PE import evidence: $relativePath" }
    foreach ($dll in $imports) {
        if ($dll -match "^(php|gmp-|mpfr-|libmpdec|vcruntime|msvcp|msvcr|concrt)") {
            throw "External language runtime dependency: $relativePath -> $dll"
        }
        if (-not (Test-Path (Join-Path $env:SystemRoot "System32\$dll"))) {
            throw "Non-system dependency: $relativePath -> $dll"
        }
    }
    Write-Host "PASS native-imports executable=$relativePath dlls=$($imports -join ',')"
}
$allowedCscDlls = @("yanflow-onnxruntime.dll", "msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")
foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File -Filter *.dll) {
    if ($file.DirectoryName -ne (Join-Path $package "csc") -or $file.Name -notin $allowedCscDlls) {
        throw "Unexpected portable runtime: $($file.FullName)"
    }
    $dependencies = (& $dumpbin /dependents $file.FullName) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect optional runtime: $($file.Name)" }
    foreach ($match in [regex]::Matches($dependencies, "(?im)^\s+([a-z0-9_.-]+\.dll)\s*$")) {
        $dll = $match.Groups[1].Value
        if ($dll -in $allowedCscDlls) {
            if (-not (Test-Path (Join-Path $file.DirectoryName $dll))) { throw "Missing app-local dependency: $dll" }
        } elseif ($dll -notmatch "^api-ms-" -and -not (Test-Path (Join-Path $env:SystemRoot "System32\$dll"))) {
            throw "Unbundled optional dependency: $dll"
        }
    }
}
if (Test-Path (Join-Path $package "php.ini")) {
    throw "Portable package contains stale runtime files"
}

function Invoke-Smoke([string]$Executable, [string]$Option, [int]$ExpectedExit) {
    $process = Start-Process -FilePath $Executable -ArgumentList $Option -WorkingDirectory $repoRoot -PassThru
    try {
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "Native smoke timed out: $Option"
        }
        if ($process.ExitCode -ne $ExpectedExit) {
            throw "Native smoke $Option returned $($process.ExitCode), expected $ExpectedExit"
        }
    } finally {
        $process.Dispose()
    }
}

# Exercise the exact portable contract, including spaces, Unicode, and a different working directory.
Compress-Archive -Path "$package\*" -DestinationPath $archive -Force
$hash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
"$hash  $([IO.Path]::GetFileName($archive))" |
    Set-Content -Encoding ascii (Join-Path $artifacts "SHA256SUMS-native.txt")
$unicodeName = -join @([char]0x8a00, [char]0x6d41)
$extracted = Join-Path $artifacts "native smoke $unicodeName $([guid]::NewGuid().ToString('N'))"
try {
    Expand-Archive -LiteralPath $archive -DestinationPath $extracted
    $yanflow = Join-Path $extracted "yanflow.exe"
    Invoke-Smoke $yanflow "--smoke" 0
    Invoke-Smoke $yanflow "--text-smoke" 0
    Invoke-Smoke $yanflow "--text-delivery-smoke" 0
    Invoke-Smoke $yanflow "--csc-pipeline-smoke" 0
    $cscWorker = Join-Path $extracted "csc\yanflow-csc-worker.exe"
    if (Test-Path $cscWorker) { Invoke-Smoke $cscWorker "--smoke" 0 }
    Invoke-Smoke $yanflow "--asr-smoke" 20
    Copy-Item -LiteralPath $sample -Destination (Join-Path $extracted "yanflow-asr-smoke.wav")
    $worker = Join-Path $extracted "funasr\yanflow-asr-worker.exe"
    Move-Item -LiteralPath $worker -Destination "$worker.disabled"
    Invoke-Smoke $yanflow "--asr-smoke" 24
    Move-Item -LiteralPath "$worker.disabled" -Destination $worker
    if (-not $env:YANFLOW_SKIP_ASR_SMOKE) { Invoke-Smoke $yanflow "--asr-smoke" 0 }
    Write-Host "PASS native-portable zip_extract=true unicode_spaces=true foreign_cwd=true missing_wave_exit=20 missing_worker_exit=24 inference=$(-not [bool]$env:YANFLOW_SKIP_ASR_SMOKE)"
    Write-Host "Native archive: $archive sha256=$hash"
} finally {
    if (Test-Path -LiteralPath $extracted) { Remove-Item -LiteralPath $extracted -Recurse -Force }
}
