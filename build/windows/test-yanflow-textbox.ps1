$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$yanflow = Join-Path $package "yanflow.exe"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
$diagnostic = Join-Path $package "yanflow-e2e-diagnostic.txt"
$ready = Join-Path $repoRoot "build\artifacts\yanflow-textbox-ready.txt"
$result = Join-Path $repoRoot "build\artifacts\yanflow-textbox-result.txt"
$targetScript = Join-Path $PSScriptRoot "yanflow-textbox-target.ps1"
foreach ($path in @($yanflow, $sample, $targetScript)) {
    if (-not (Test-Path $path)) { throw "Textbox E2E dependency missing: $path" }
}
Remove-Item -Force -ErrorAction SilentlyContinue $ready, $result, $diagnostic
Copy-Item -Force $sample $fixture
$target = $null
try {
    $targetArguments = '-Sta -NoProfile -ExecutionPolicy Bypass -File "{0}" -ReadyFile "{1}" -ResultFile "{2}"' -f $targetScript, $ready, $result
    $target = Start-Process powershell.exe -ArgumentList $targetArguments -PassThru
    $deadline = (Get-Date).AddSeconds(10)
    while (-not (Test-Path $ready) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if (-not (Test-Path $ready)) { throw "Isolated editable target did not become ready" }
    $shell = New-Object -ComObject WScript.Shell
    if (-not $shell.AppActivate("YanFlow isolated editable target")) { throw "Could not focus isolated editable target" }
    Start-Sleep -Milliseconds 300

    $process = Start-Process $yanflow -ArgumentList "--e2e-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow editable-target E2E timed out"
    }
    if ($process.ExitCode -ne 0) { throw "YanFlow E2E exited with $($process.ExitCode)" }
    if (-not $target.WaitForExit(5000)) {
        $details = if (Test-Path $diagnostic) { [IO.File]::ReadAllText($diagnostic) } else { "no YanFlow diagnostic" }
        throw "Target did not observe injected text: $details"
    }
    if (-not (Test-Path $result)) { throw "Target produced no injection evidence" }
    $actual = [IO.File]::ReadAllText($result, [Text.Encoding]::UTF8)
    Write-Host "PASS yanflow-textbox-e2e text=$actual"
} finally {
    if ($null -ne $target -and -not $target.HasExited) { $target.Kill() }
    if ($null -ne $target) { $target.Dispose() }
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture, $ready, $result, $diagnostic
}
