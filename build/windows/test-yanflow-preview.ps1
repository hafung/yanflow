$ErrorActionPreference = "Stop"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repo "build\artifacts\yanflow-windows-x64"
$exe = Join-Path $package "yanflow.exe"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
$result = Join-Path $package "yanflow-preview-result.txt"
$sample = Join-Path $repo "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
try {
    Copy-Item -LiteralPath $sample -Destination $fixture -Force
    $process = Start-Process -FilePath $exe -ArgumentList "--preview-pipeline-smoke" -PassThru
    try {
        if (-not $process.WaitForExit(25000)) { $process.Kill(); throw "Preview pipeline timed out" }
        $details = if (Test-Path $result) { [IO.File]::ReadAllText($result) } else { "No final result" }
        if ($process.ExitCode -ne 0) { throw "Preview pipeline returned $($process.ExitCode): $details" }
        Write-Host "PASS held PCM replay: partial recognition before release, visible caret bubble, focus retained, no early insertion, one final delivery, preview dismissed"
        Write-Host $details
    } finally { $process.Dispose() }
} finally {
    Remove-Item -LiteralPath $fixture, $result -Force -ErrorAction SilentlyContinue
}
