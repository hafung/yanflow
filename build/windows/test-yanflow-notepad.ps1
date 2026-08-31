$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$yanflow = Join-Path $package "yanflow.exe"
$sample = Join-Path $repoRoot "build\deps\yanflow\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav"
$fixture = Join-Path $package "yanflow-asr-smoke.wav"
$targetFile = Join-Path $repoRoot "build\artifacts\yanflow-notepad-smoke.txt"
foreach ($path in @($yanflow, $sample)) {
    if (-not (Test-Path $path)) { throw "Notepad smoke dependency missing: $path" }
}
if (Get-Process notepad -ErrorAction SilentlyContinue) {
    throw "Close existing Notepad windows before this isolated smoke; the test will not touch an existing session."
}

$savedClipboard = [Windows.Forms.Clipboard]::GetDataObject()
$notepad = $null
try {
    Set-Content -Encoding utf8 -NoNewline $targetFile "YANFLOW-E2E:"
    Copy-Item -Force $sample $fixture
    $startedAt = Get-Date
    [void](Start-Process notepad.exe -ArgumentList "`"$targetFile`"")
    $deadline = (Get-Date).AddSeconds(10)
    do {
        Start-Sleep -Milliseconds 100
        $notepad = Get-Process notepad -ErrorAction SilentlyContinue |
            Where-Object { $_.StartTime -ge $startedAt.AddSeconds(-1) -and $_.MainWindowHandle -ne 0 } |
            Select-Object -First 1
    } while ($null -eq $notepad -and (Get-Date) -lt $deadline)
    if ($null -eq $notepad) { throw "Notepad did not create an editable window" }

    $shell = New-Object -ComObject WScript.Shell
    if (-not $shell.AppActivate($notepad.Id)) { throw "Could not focus the isolated Notepad window" }
    Start-Sleep -Milliseconds 250
    [Windows.Forms.SendKeys]::SendWait("^{END}")

    $process = Start-Process $yanflow -ArgumentList "--e2e-smoke" -PassThru
    if (-not $process.WaitForExit(15000)) {
        $process.Kill()
        throw "YanFlow E2E smoke timed out"
    }
    if ($process.ExitCode -ne 0) { throw "YanFlow E2E smoke exited with $($process.ExitCode)" }

    [void]$shell.AppActivate($notepad.Id)
    Start-Sleep -Milliseconds 250
    [Windows.Forms.SendKeys]::SendWait("^a")
    [Windows.Forms.SendKeys]::SendWait("^c")
    Start-Sleep -Milliseconds 250
    $actual = [Windows.Forms.Clipboard]::GetText()
    $expected = -join @([char]0x6ee8, [char]0x6d77, [char]0x65b0, [char]0x533a, [char]0x6709, [char]0x623f)
    if (-not $actual.Contains("YANFLOW-E2E:") -or -not $actual.Contains($expected)) {
        throw "Recognized Unicode text was not injected into Notepad. Actual: $actual"
    }
    Write-Host "PASS yanflow-notepad-e2e text=$actual"
} finally {
    if ($null -ne $notepad) {
        Stop-Process -Id $notepad.Id -Force -ErrorAction SilentlyContinue
    }
    Remove-Item -Force -ErrorAction SilentlyContinue $fixture, $targetFile
    if ($null -ne $savedClipboard) {
        [Windows.Forms.Clipboard]::SetDataObject($savedClipboard, $true)
    }
}
