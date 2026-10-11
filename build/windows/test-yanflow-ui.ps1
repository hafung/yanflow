$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$exe = Join-Path $repo "build\artifacts\yanflow-windows-x64\yanflow.exe"
$clipboard = [Windows.Forms.Clipboard]::GetDataObject()
try {
    foreach ($option in @("--ui-state-smoke", "--ui-preview-smoke", "--learn-smoke", "--dictionary-manager-smoke")) {
        $process = Start-Process -FilePath $exe -ArgumentList $option -PassThru
        try {
            if (-not $process.WaitForExit(10000)) { $process.Kill(); throw "UI smoke timed out: $option" }
            if ($process.ExitCode -ne 0) { throw "UI smoke $option returned $($process.ExitCode)" }
        } finally { $process.Dispose() }
    }
    Write-Host "PASS delivered text stays collapsed, short/long adaptive bounds, copy/close/new text state, hold-to-realtime toggle, legacy/custom hotkey migration"
    Write-Host "PASS dictionary manager add/edit/delete/search/preview/scopes, malformed line preservation, conflict/UTF-8/lock/stale-write guards"
} finally {
    if ($null -ne $clipboard) { [Windows.Forms.Clipboard]::SetDataObject($clipboard, $true) }
    else { [Windows.Forms.Clipboard]::Clear() }
    foreach ($name in @("collapsed", "bubble", "short", "long", "copied")) {
        Remove-Item -LiteralPath (Join-Path ([IO.Path]::GetDirectoryName($exe)) "yanflow-ui-$name.bmp") -Force -ErrorAction SilentlyContinue
    }
}
