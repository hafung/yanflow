$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$exe = Join-Path $repo "build\artifacts\yanflow-windows-x64\yanflow.exe"
$savedClipboard = [Windows.Forms.Clipboard]::GetDataObject()
try {
    foreach ($option in @("--accuracy-smoke", "--learn-smoke")) {
        $process = Start-Process -FilePath $exe -ArgumentList $option -PassThru
        try {
            if (-not $process.WaitForExit(10000)) { $process.Kill(); throw "Accuracy smoke timed out: $option" }
            if ($process.ExitCode -ne 0) { throw "Accuracy smoke $option returned $($process.ExitCode)" }
        } finally { $process.Dispose() }
    }
    Write-Host "PASS adaptive segmentation ownership/bounds, short speech gates, dictionary persistence, native correction dialog, app isolation"
    & py.exe -3 (Join-Path $repo "tests\accuracy-worker-smoke.py") $repo
    if ($LASTEXITCODE -ne 0) { throw "Native ASR timing/short-PCM smoke failed" }
} finally {
    if ($null -ne $savedClipboard) { [Windows.Forms.Clipboard]::SetDataObject($savedClipboard, $true) }
    else { [Windows.Forms.Clipboard]::Clear() }
}
