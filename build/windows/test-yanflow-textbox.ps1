$ErrorActionPreference = "Stop"
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TextboxSmokeFocus {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint first, uint second, bool attach);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr SetFocus(IntPtr window);
}
'@

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
    # A companion console can receive AppActivate instead of the WinForms
    # editor. Create no console; do not hide the editor's first ShowWindow.
    $targetInfo = [Diagnostics.ProcessStartInfo]::new()
    $targetInfo.FileName = "powershell.exe"
    $targetInfo.Arguments = $targetArguments
    $targetInfo.UseShellExecute = $false
    $targetInfo.CreateNoWindow = $true
    $target = [Diagnostics.Process]::Start($targetInfo)
    $deadline = (Get-Date).AddSeconds(10)
    while (-not (Test-Path $ready) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
    if (-not (Test-Path $ready)) { throw "Isolated editable target did not become ready" }
    $handles = [IO.File]::ReadAllText($ready) | ConvertFrom-Json
    $targetWindow = [IntPtr][long]$handles.window
    $targetEditor = [IntPtr][long]$handles.editor
    [uint32]$windowPid = 0
    $targetThread = [TextboxSmokeFocus]::GetWindowThreadProcessId($targetWindow, [ref]$windowPid)
    if ($windowPid -ne $target.Id) { throw "Isolated target handle did not belong to its process" }
    $currentThread = [TextboxSmokeFocus]::GetCurrentThreadId()
    [uint32]$foregroundPid = 0
    $foregroundThread = [TextboxSmokeFocus]::GetWindowThreadProcessId([TextboxSmokeFocus]::GetForegroundWindow(), [ref]$foregroundPid)
    $attachedForeground = $false
    $attachedTarget = $false
    try {
        if ($foregroundThread -and $foregroundThread -ne $currentThread) {
            $attachedForeground = [TextboxSmokeFocus]::AttachThreadInput($currentThread, $foregroundThread, $true)
        }
        if ($targetThread -ne $currentThread -and $targetThread -ne $foregroundThread) {
            $attachedTarget = [TextboxSmokeFocus]::AttachThreadInput($currentThread, $targetThread, $true)
        }
        [void][TextboxSmokeFocus]::ShowWindow($targetWindow, 9)
        [void][TextboxSmokeFocus]::SetForegroundWindow($targetWindow)
        [void][TextboxSmokeFocus]::SetFocus($targetEditor)
    } finally {
        if ($attachedTarget) { [void][TextboxSmokeFocus]::AttachThreadInput($currentThread, $targetThread, $false) }
        if ($attachedForeground) { [void][TextboxSmokeFocus]::AttachThreadInput($currentThread, $foregroundThread, $false) }
    }
    Start-Sleep -Milliseconds 100
    if ([TextboxSmokeFocus]::GetForegroundWindow() -ne $targetWindow) { throw "Desktop focus moved away from the isolated target" }

    $process = Start-Process $yanflow -ArgumentList "--e2e-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow editable-target E2E timed out"
    }
    if ($process.ExitCode -ne 0) {
        $details = if (Test-Path $diagnostic) { [IO.File]::ReadAllText($diagnostic) } else { "no YanFlow diagnostic" }
        $focusDetails = if (Test-Path "$ready.focus.json") { [IO.File]::ReadAllText("$ready.focus.json") } else { "no target focus diagnostic" }
        throw "YanFlow E2E exited with $($process.ExitCode): $details $focusDetails"
    }
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
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture, $ready, $result, $diagnostic, "$ready.focus.json"
}
