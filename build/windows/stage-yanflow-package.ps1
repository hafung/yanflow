$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$dependencies = Join-Path $repoRoot "build\deps\yanflow"
$package = Join-Path $repoRoot "build\artifacts\yanflow-windows-x64"
$staging = "$package.partial"

# Stage an explicit file list; a previous TypePHP build must leave no runtime behind.
$files = @{
    "yanflow.exe" = Join-Path $repoRoot "build\artifacts\yanflow.exe"
    "funasr\yanflow-asr-worker.exe" = Join-Path $dependencies "funasr\yanflow-asr-worker.exe"
    "models\sensevoice-small-q8.gguf" = Join-Path $dependencies "models\sensevoice-small-q8.gguf"
    "models\fsmn-vad.gguf" = Join-Path $dependencies "models\fsmn-vad.gguf"
    "THIRD-PARTY-NOTICES.md" = Join-Path $repoRoot "THIRD-PARTY-NOTICES.md"
    "THIRD-PARTY-NOTICES-worker.txt" = Join-Path $repoRoot "app\asr-worker\THIRD-PARTY-NOTICES.txt"
    "LICENSE-MIT-native-core.txt" = Join-Path $repoRoot "LICENSES\MIT-native-core.txt"
    "LICENSE-YanFlow.txt" = Join-Path $repoRoot "LICENSE"
    "README.md" = Join-Path $repoRoot "README.md"
    "dictionary.tsv" = Join-Path $repoRoot "assets\dictionary.tsv"
    "dictionary-development.tsv" = Join-Path $repoRoot "assets\dictionary-development.tsv"
    "docs\text-correction.md" = Join-Path $repoRoot "docs\text-correction.md"
}
# Every portable package includes the complete, verified CSC bundle.
$csc = Join-Path $dependencies "csc"
if (-not (Test-Path -LiteralPath $csc)) { throw "Missing CSC bundle; run build\windows\build-yanflow.cmd" }
if (Test-Path -LiteralPath $csc) {
    foreach ($required in @("yanflow-csc-worker.exe", "yanflow-onnxruntime.dll", "model.onnx", "vocab.txt", "SHA256SUMS", "LICENSE-onnxruntime.txt", "NOTICE-onnxruntime.txt", "README-model.md", "LICENSE-model.txt", "NOTICES.md", "msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
        if (-not (Test-Path -LiteralPath (Join-Path $csc $required))) { throw "Incomplete CSC bundle: $required" }
    }
    foreach ($entry in Get-ChildItem -LiteralPath $csc -File) {
        if ($entry.Name -ne "SHA256SUMS") {
            $expected = (Get-Content -LiteralPath (Join-Path $csc "SHA256SUMS") | Where-Object { $_.EndsWith("  " + $entry.Name) })
            $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $entry.FullName).Hash.ToLowerInvariant()
            if (-not $expected -or $expected.Substring(0, 64) -ne $actual) { throw "CSC bundle checksum mismatch: $($entry.Name)" }
        }
        $files["csc\" + $entry.Name] = $entry.FullName
    }
}
foreach ($source in $files.Values) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf) -or (Get-Item -LiteralPath $source).Length -eq 0) {
        throw "Package input missing or empty: $source"
    }
}
if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
New-Item -ItemType Directory -Path $staging | Out-Null
foreach ($relativePath in $files.Keys) {
    $destination = Join-Path $staging $relativePath
    New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
    Copy-Item -LiteralPath $files[$relativePath] -Destination $destination
}
if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
Move-Item -LiteralPath $staging -Destination $package
Write-Host "Staged native portable package: $package"
