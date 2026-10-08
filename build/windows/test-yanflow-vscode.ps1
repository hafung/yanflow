$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class YanFlowVsCodeFocus {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
}
"@

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$yanflow = Join-Path $package "yanflow.exe"
$code = "C:\Users\admin\AppData\Local\Programs\Microsoft VS Code\Code.exe"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
$diagnostic = Join-Path $package "yanflow-e2e-diagnostic.txt"
$testRoot = Join-Path $repoRoot "build\artifacts\yanflow-vscode-test"
$profile = Join-Path $testRoot "profile"
$extensions = Join-Path $testRoot "extensions"
$document = Join-Path $testRoot "input.txt"
foreach ($path in @($yanflow, $code, $sample)) {
    if (-not (Test-Path $path)) { throw "VS Code E2E dependency missing: $path" }
}

function Get-IsolatedCodeProcesses {
    @(Get-CimInstance Win32_Process -Filter "Name='Code.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $null -ne $_.CommandLine -and $_.CommandLine.Contains($profile) })
}

$editor = $null
try {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $testRoot
    Remove-Item -Force -ErrorAction SilentlyContinue $diagnostic
    New-Item -ItemType Directory -Force $profile, $extensions | Out-Null
    [IO.File]::WriteAllText($document, "YANFLOW-VSCODE:", [Text.UTF8Encoding]::new($false))
    Copy-Item -Force $sample $fixture
    $arguments = '--user-data-dir="{0}" --extensions-dir="{1}" --disable-extensions --skip-welcome --new-window "{2}"' -f $profile, $extensions, $document
    [void](Start-Process $code -ArgumentList $arguments)

    $deadline = (Get-Date).AddSeconds(20)
    do {
        Start-Sleep -Milliseconds 100
        foreach ($candidate in Get-IsolatedCodeProcesses) {
            $process = Get-Process -Id $candidate.ProcessId -ErrorAction SilentlyContinue
            if ($null -ne $process -and $process.MainWindowHandle -ne 0) {
                $editor = $process
                break
            }
        }
    } while ($null -eq $editor -and (Get-Date) -lt $deadline)
    if ($null -eq $editor) { throw "Isolated VS Code window did not become ready" }

    [void][YanFlowVsCodeFocus]::ShowWindow($editor.MainWindowHandle, 5)
    if (-not [YanFlowVsCodeFocus]::SetForegroundWindow($editor.MainWindowHandle)) {
        throw "Could not focus isolated VS Code window"
    }
    # A top-level Electron window can appear before its editor is ready. Wait for
    # an editable focus instead of relying on a fixed delay.
    $editableReady = $false
    $deadline = (Get-Date).AddSeconds(15)
    do {
        [void][YanFlowVsCodeFocus]::SetForegroundWindow($editor.MainWindowHandle)
        [Windows.Forms.SendKeys]::SendWait("^1")
        [Windows.Forms.SendKeys]::SendWait("^{END}")
        Start-Sleep -Milliseconds 200
        $focused = [Windows.Automation.AutomationElement]::FocusedElement
        if ($null -ne $focused -and
            [YanFlowVsCodeFocus]::GetForegroundWindow() -eq $editor.MainWindowHandle) {
            $type = $focused.Current.ControlType
            $editableReady = $type -eq [Windows.Automation.ControlType]::Edit -or
                $type -eq [Windows.Automation.ControlType]::Document
        }
    } while (-not $editableReady -and (Get-Date) -lt $deadline)
    if (-not $editableReady) { throw "Isolated VS Code editor did not acquire editable focus" }

    $process = Start-Process $yanflow -ArgumentList "--e2e-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow VS Code E2E timed out"
    }
    if ($process.ExitCode -ne 0) {
        $details = if (Test-Path $diagnostic) { [IO.File]::ReadAllText($diagnostic) } else { "no YanFlow diagnostic" }
        throw "YanFlow VS Code E2E exited with $($process.ExitCode): $details"
    }

    [void][YanFlowVsCodeFocus]::SetForegroundWindow($editor.MainWindowHandle)
    Start-Sleep -Milliseconds 200
    [Windows.Forms.SendKeys]::SendWait("^s")
    Start-Sleep -Milliseconds 700
    $actual = [IO.File]::ReadAllText($document, [Text.Encoding]::UTF8)
    $expected = -join @([char]0x6ee8, [char]0x6d77, [char]0x65b0, [char]0x533a, [char]0x6709, [char]0x623f)
    if (-not $actual.Contains($expected)) {
        $details = if (Test-Path $diagnostic) { [IO.File]::ReadAllText($diagnostic) } else { "no YanFlow diagnostic" }
        throw "Recognized text was not injected into VS Code editor. Actual=$actual Diagnostic=$details"
    }
    Write-Host "PASS yanflow-vscode-editor text=$actual"
} finally {
    foreach ($isolated in Get-IsolatedCodeProcesses) {
        Stop-Process -Id $isolated.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 250
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $testRoot
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture, $diagnostic
}
