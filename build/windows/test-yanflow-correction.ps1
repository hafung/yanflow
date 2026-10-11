$ErrorActionPreference = "Stop"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$package = Join-Path $repo "build\artifacts\yanflow-windows-x64"
$temporary = Join-Path ([IO.Path]::GetTempPath()) "YanFlow correction $([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $temporary | Out-Null
function Invoke-Correction([string]$Executable, [string[]]$Arguments, [int]$Expected) {
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru
    try {
        if (-not $process.WaitForExit(45000)) { $process.Kill(); throw "Correction smoke timed out" }
        if ($process.ExitCode -ne $Expected) { throw "Correction returned $($process.ExitCode), expected $Expected" }
    } finally { $process.Dispose() }
}
try {
    $exe = Join-Path $temporary "yanflow.exe"
    Copy-Item -LiteralPath (Join-Path $package "yanflow.exe") -Destination $exe
    Copy-Item -LiteralPath (Join-Path $package "dictionary.tsv") -Destination $temporary
    Invoke-Correction $exe @("--text-smoke") 0
    Invoke-Correction $exe @("--text-delivery-smoke") 0
    Invoke-Correction (Join-Path $package "yanflow.exe") @("--csc-pipeline-smoke") 0
    $input = Join-Path $temporary "input.txt"
    $output = Join-Path $temporary "output.txt"
    [IO.File]::WriteAllText($input, "openai api 123 `"asr`"", [Text.UTF8Encoding]::new($false))
    $arguments = @("--correct-text", "`"$input`"", "`"$output`"")
    Invoke-Correction $exe $arguments 0
    $expected = "OpenAI API 123 `"asr`""
    if ([IO.File]::ReadAllText($output) -ne $expected) { throw "Dictionary/protected-text mismatch" }
    Invoke-Correction $exe ($arguments + "--macbert") 32
    if (-not [IO.File]::ReadAllText("$output.diagnostic.txt").Contains("yanflow-csc-worker.exe")) { throw "Missing CSC failure reason was lost" }
    if ([IO.File]::ReadAllText($output) -ne $expected) { throw "Missing CSC worker did not preserve dictionary output" }
    # Start the real worker with an invalid model, so its stderr/exit diagnosis
    # is verified through the GUI executable rather than only checking files.
    $csc = Join-Path $temporary "csc"
    New-Item -ItemType Directory -Path $csc | Out-Null
    foreach ($name in @("yanflow-csc-worker.exe", "yanflow-onnxruntime.dll", "vocab.txt", "msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
        Copy-Item -LiteralPath (Join-Path $package "csc\$name") -Destination $csc
    }
    [IO.File]::WriteAllText((Join-Path $csc "model.onnx"), "invalid-model")
    Invoke-Correction $exe ($arguments + "--macbert") 32
    $failure = [IO.File]::ReadAllText("$output.diagnostic.txt")
    if (-not $failure.Contains("yanflow-csc:") -or -not $failure.Contains("model.onnx")) { throw "Worker initialization details were lost: $failure" }
    if ([IO.File]::ReadAllText($output) -ne $expected) { throw "Broken CSC model did not preserve dictionary output" }
    [IO.File]::WriteAllBytes((Join-Path $temporary "dictionary.tsv"), [byte[]]@(0xff, 0xfe, 0xff))
    Invoke-Correction $exe $arguments 0
    if ([IO.File]::ReadAllText($output) -ne [IO.File]::ReadAllText($input)) { throw "Invalid UTF-8 dictionary did not fail closed" }
    Invoke-Correction $exe @("--correct-text", "`"$temporary\missing.txt`"", "`"$output`"") 30
    Write-Host "PASS correction guards, Unicode delivery, actual CSC pipeline/reuse/reset/retry, missing CSC diagnostics/fallback, invalid dictionary, missing input"
} finally { Remove-Item -LiteralPath $temporary -Recurse -Force }
