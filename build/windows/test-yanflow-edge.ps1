$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class YanFlowWindowFocus {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
}
"@

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$yanflow = Join-Path $package "yanflow.exe"
$edge = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
$diagnostic = Join-Path $package "yanflow-e2e-diagnostic.txt"
$profile = Join-Path $repoRoot "build\artifacts\yanflow-edge-profile"
foreach ($path in @($yanflow, $edge, $sample)) {
    if (-not (Test-Path $path)) { throw "Edge E2E dependency missing: $path" }
}

function Get-IsolatedEdgeProcesses {
    @(Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $null -ne $_.CommandLine -and $_.CommandLine.Contains($profile) })
}

$savedClipboard = [Windows.Forms.Clipboard]::GetDataObject()
$browser = $null
try {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $profile
    Remove-Item -Force -ErrorAction SilentlyContinue $diagnostic
    New-Item -ItemType Directory -Force $profile | Out-Null
    Copy-Item -Force $sample $fixture
    $arguments = '--user-data-dir="{0}" --no-first-run --no-default-browser-check --disable-sync --disable-features=msEdgeSidebarV2 --new-window about:blank' -f $profile
    [void](Start-Process $edge -ArgumentList $arguments)

    $deadline = (Get-Date).AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 100
        foreach ($candidate in Get-IsolatedEdgeProcesses) {
            $process = Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
            if ($null -ne $process -and $process.MainWindowHandle -ne 0) {
                $browser = $process
                break
            }
        }
    } while ($null -eq $browser -and (Get-Date) -lt $deadline)
    if ($null -eq $browser) { throw "Isolated Edge window did not become ready" }

    [void][YanFlowWindowFocus]::ShowWindow($browser.MainWindowHandle, 5)
    if (-not [YanFlowWindowFocus]::SetForegroundWindow($browser.MainWindowHandle)) {
        throw "Could not focus isolated Edge window"
    }
    Start-Sleep -Milliseconds 300
    [Windows.Forms.SendKeys]::SendWait("^l")
    Start-Sleep -Milliseconds 200

    $process = Start-Process $yanflow -ArgumentList "--e2e-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow Edge E2E timed out"
    }
    if ($process.ExitCode -ne 0) { throw "YanFlow Edge E2E exited with $($process.ExitCode)" }

    [void][YanFlowWindowFocus]::SetForegroundWindow($browser.MainWindowHandle)
    Start-Sleep -Milliseconds 200
    [Windows.Forms.SendKeys]::SendWait("^l")
    [Windows.Forms.SendKeys]::SendWait("^c")
    Start-Sleep -Milliseconds 200
    $actual = [Windows.Forms.Clipboard]::GetText()
    $expected = -join @([char]0x6ee8, [char]0x6d77, [char]0x65b0, [char]0x533a, [char]0x6709, [char]0x623f)
    if (-not $actual.Contains($expected)) {
        $details = if (Test-Path $diagnostic) { [IO.File]::ReadAllText($diagnostic) } else { "no YanFlow diagnostic" }
        throw "Recognized text was not injected into Edge address bar. Actual=$actual Diagnostic=$details"
    }
    Write-Host "PASS yanflow-edge-address-bar text=$actual"
} finally {
    foreach ($isolated in Get-IsolatedEdgeProcesses) {
        Stop-Process -Id $isolated.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 250
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $profile
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture, $diagnostic
    if ($null -ne $savedClipboard) {
        [Windows.Forms.Clipboard]::SetDataObject($savedClipboard, $true)
    } else {
        [Windows.Forms.Clipboard]::Clear()
    }
}
