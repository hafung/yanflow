param(
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class YanFlowCaptureBounds {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect bounds);
}
'@
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $repoRoot "assets\screenshots\yanflow-running.png"
}
Copy-Item -Force $sample $fixture
$process = Start-Process (Join-Path $package "yanflow.exe") -ArgumentList "--readme-demo" -PassThru
try {
    Start-Sleep -Seconds 7
    $process.Refresh()
    $bounds = [YanFlowCaptureBounds+Rect]::new()
    if (-not [YanFlowCaptureBounds]::GetWindowRect($process.MainWindowHandle, [ref]$bounds)) { throw "YanFlow preview window is unavailable" }
    $width = $bounds.Right - $bounds.Left
    $height = $bounds.Bottom - $bounds.Top
    $bitmap = [Drawing.Bitmap]::new($width, $height)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bitmap.Size)
        $bitmap.Save([IO.Path]::GetFullPath($Output), [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
} finally {
    if (-not $process.HasExited) { $process.Kill() }
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture
}
Write-Host "Captured actual YanFlow window: $Output"
