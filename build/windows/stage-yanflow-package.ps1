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
