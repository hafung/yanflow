param(
    [string]$Destination = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repoRoot "build\deps\yanflow"
}
$Destination = [IO.Path]::GetFullPath($Destination)
$sourceDir = Join-Path $Destination "source"
$downloadDir = Join-Path $Destination "downloads"
$buildDir = Join-Path $repoRoot "build\native\yanflow-worker"
New-Item -ItemType Directory -Force $sourceDir, $downloadDir, $buildDir | Out-Null

$senseVoiceUrl = "https://github.com/QwenAudio/SenseVoice/archive/refs/tags/runtime-llamacpp-v0.1.9.zip"
$senseVoiceSha256 = "0c9a19f15c279cd078ac2b8fbc57dea87530dce66a7d38a0a68e7047e51b9b06"
$llamaCommit = "8086439a4cea94c71a5dfb8fe4ad1546aebd640f"
$llamaUrl = "https://github.com/ggml-org/llama.cpp/archive/$llamaCommit.zip"
$llamaSha256 = "194af8420bad2150d625751622db85494cdd22512fa09dfb8a23e4c2778a0341"

function Get-Archive([string]$Url, [string]$Path, [string]$Sha256) {
    if (-not (Test-Path $Path)) {
        $partial = "$Path.partial"
        Write-Host "Downloading: $Url"
        & curl.exe --fail --location --retry 5 --retry-delay 2 --continue-at - --output $partial $Url
        if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE" }
        Move-Item -Force $partial $Path
    }
    $actual = (Get-FileHash -Algorithm SHA256 $Path).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "Source archive checksum mismatch: $Path ($actual)" }
}

$senseVoiceZip = Join-Path $downloadDir "sensevoice-runtime-llamacpp-v0.1.9.zip"
$llamaZip = Join-Path $downloadDir "llama-$llamaCommit.zip"
Get-Archive $senseVoiceUrl $senseVoiceZip $senseVoiceSha256
Get-Archive $llamaUrl $llamaZip $llamaSha256

$senseVoiceExtract = Join-Path $sourceDir "sensevoice-v0.1.9"
$llamaExtract = Join-Path $sourceDir "llama-$llamaCommit"
if (-not (Test-Path $senseVoiceExtract)) {
    $temporary = "$senseVoiceExtract.partial"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $temporary
    New-Item -ItemType Directory -Force $temporary | Out-Null
    Expand-Archive -Force $senseVoiceZip $temporary
    $root = Get-ChildItem -Directory $temporary | Select-Object -First 1
    Move-Item $root.FullName $senseVoiceExtract
    Remove-Item -Recurse -Force $temporary
}
if (-not (Test-Path $llamaExtract)) {
    $temporary = "$llamaExtract.partial"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $temporary
    New-Item -ItemType Directory -Force $temporary | Out-Null
    Expand-Archive -Force $llamaZip $temporary
    $root = Get-ChildItem -Directory $temporary | Select-Object -First 1
    Move-Item $root.FullName $llamaExtract
    Remove-Item -Recurse -Force $temporary
}

$cmakeCommand = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
if ($null -eq $cmakeCommand -and $env:VS_BUILD_TOOLS) {
    $bundledCmake = Join-Path $env:VS_BUILD_TOOLS "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $bundledCmake) { $cmakeCommand = Get-Item $bundledCmake }
}
if ($null -eq $cmakeCommand) { throw "CMake not found. Install CMake or set VS_BUILD_TOOLS." }
$cmake = $cmakeCommand.Source
$funasrRuntime = Join-Path $senseVoiceExtract "runtime\llama.cpp"
$project = Join-Path $repoRoot "app\asr-worker"

& $cmake --fresh -S $project -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DFUNASR_RUNTIME_DIR=$funasrRuntime" "-DLLAMA_CPP_DIR=$llamaExtract"
if ($LASTEXITCODE -ne 0) { throw "YanFlow worker CMake configure failed" }
& $cmake --build $buildDir --target yanflow-asr-worker
if ($LASTEXITCODE -ne 0) { throw "YanFlow worker build failed" }

$worker = Join-Path $buildDir "yanflow-asr-worker.exe"
$runtimeDir = Join-Path $Destination "funasr"
New-Item -ItemType Directory -Force $runtimeDir | Out-Null
if (-not (Test-Path $worker)) { throw "Worker artifact missing: $worker" }
Copy-Item -Force $worker (Join-Path $runtimeDir "yanflow-asr-worker.exe")
Write-Host "YanFlow persistent worker: $runtimeDir\yanflow-asr-worker.exe"
