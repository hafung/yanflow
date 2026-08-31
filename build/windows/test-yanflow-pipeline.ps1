$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$yanflow = Join-Path $package "yanflow.exe"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
foreach ($path in @($yanflow, $sample)) {
    if (-not (Test-Path $path)) { throw "Pipeline smoke dependency missing: $path" }
}
$savedClipboard = [Windows.Forms.Clipboard]::GetDataObject()
try {
    Copy-Item -Force $sample $fixture
    $process = Start-Process $yanflow -ArgumentList "--pipeline-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow capture-to-endpoint pipeline timed out"
    }
    if ($process.ExitCode -ne 0) { throw "YanFlow pipeline smoke exited with $($process.ExitCode)" }
    $actual = [Windows.Forms.Clipboard]::GetText()
    if ([string]::IsNullOrWhiteSpace($actual)) { throw "Pipeline produced an empty transcript" }
    $expected = -join @([char]0x6ee8, [char]0x6d77, [char]0x65b0, [char]0x533a, [char]0x6709, [char]0x623f)
    if (-not $actual.Contains($expected)) { throw "Pipeline transcript was incomplete: $actual" }
    Write-Host "PASS yanflow-pipeline ring=pcm16 input_level=25pct endpoint=stop-flush asr=sensevoice text=$actual"
} finally {
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture
    if ($null -ne $savedClipboard) {
        [Windows.Forms.Clipboard]::SetDataObject($savedClipboard, $true)
    } else {
        [Windows.Forms.Clipboard]::Clear()
    }
}
