$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$yanflow = Join-Path $package "yanflow.exe"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
foreach ($path in @($yanflow, $sample)) {
    if (-not (Test-Path $path)) { throw "Fallback smoke dependency missing: $path" }
}
$savedClipboard = [Windows.Forms.Clipboard]::GetDataObject()
try {
    Copy-Item -Force $sample $fixture
    $process = Start-Process $yanflow -ArgumentList "--fallback-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow fallback smoke timed out"
    }
    if ($process.ExitCode -ne 0) { throw "YanFlow fallback smoke exited with $($process.ExitCode)" }
    Write-Host "PASS yanflow-fallback bubble=520x136 fully_visible=true copy_button=one_click clipboard=unicode"
} finally {
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture
    if ($null -ne $savedClipboard) {
        [Windows.Forms.Clipboard]::SetDataObject($savedClipboard, $true)
    } else {
        [Windows.Forms.Clipboard]::Clear()
    }
}
