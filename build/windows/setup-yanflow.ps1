param(
    [string]$Destination = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $repoRoot "build\deps\yanflow"
}
$Destination = [IO.Path]::GetFullPath($Destination)
$runtimeDir = Join-Path $Destination "funasr"
$modelDir = Join-Path $Destination "models"
$downloadDir = Join-Path $Destination "downloads"
New-Item -ItemType Directory -Force $runtimeDir, $modelDir, $downloadDir | Out-Null

$runtimeUrl = "https://github.com/QwenAudio/SenseVoice/releases/download/runtime-llamacpp-v0.1.9/funasr-llamacpp-windows-x64.zip"
$runtimeSha256 = "6767af74e42c8b928742e12d5995c139636d9482ea151cdbb51f1b7573667772"
$senseVoiceCommit = "90c1c61912018b70ada0fcc024ea24aca62f2e63"
$vadCommit = "6840bae4c5c92ee8c04faaf4db23dd0105098d7f"
$senseVoiceUrl = "https://huggingface.co/FunAudioLLM/SenseVoiceSmall-GGUF/resolve/$senseVoiceCommit/sensevoice-small-q8.gguf"
$vadUrl = "https://huggingface.co/FunAudioLLM/fsmn-vad-GGUF/resolve/$vadCommit/fsmn-vad.gguf"
$senseVoiceSha256 = "4ae45c94422de949b387e2e0fb10d7e14e4c42c69db30c3444ecc7d4b844b7c5"
$vadSha256 = "1270f2559c495f4e7b6e739541151027d360761a3fda43fc147034f5719f5479"

function Get-Resource([string]$Url, [string]$Path) {
    if (Test-Path $Path) {
        Write-Host "Present: $Path"
        return
    }
    $partial = "$Path.partial"
    Write-Host "Downloading: $Url"
    $curl = Get-Command "curl.exe" -ErrorAction SilentlyContinue
    if ($null -ne $curl) {
        & $curl.Source --fail --location --retry 5 --retry-delay 2 --continue-at - --output $partial $Url
        if ($LASTEXITCODE -ne 0) {
            throw "curl failed with exit code $LASTEXITCODE while downloading $Url"
        }
    } else {
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $partial
    }
    Move-Item -Force $partial $Path
}

$runtimeZip = Join-Path $downloadDir "funasr-llamacpp-windows-x64-v0.1.9.zip"
Get-Resource $runtimeUrl $runtimeZip
$actualRuntimeSha256 = (Get-FileHash -Algorithm SHA256 $runtimeZip).Hash.ToLowerInvariant()
if ($actualRuntimeSha256 -ne $runtimeSha256) {
    throw "FunASR runtime checksum mismatch: $actualRuntimeSha256"
}
if (-not (Test-Path (Join-Path $runtimeDir "llama-funasr-sensevoice.exe"))) {
    Expand-Archive -Force $runtimeZip $runtimeDir
}

Get-Resource $senseVoiceUrl (Join-Path $modelDir "sensevoice-small-q8.gguf")
Get-Resource $vadUrl (Join-Path $modelDir "fsmn-vad.gguf")

$actualSenseVoiceSha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $modelDir "sensevoice-small-q8.gguf")).Hash.ToLowerInvariant()
if ($actualSenseVoiceSha256 -ne $senseVoiceSha256) {
    throw "SenseVoice model checksum mismatch: $actualSenseVoiceSha256"
}
$actualVadSha256 = (Get-FileHash -Algorithm SHA256 (Join-Path $modelDir "fsmn-vad.gguf")).Hash.ToLowerInvariant()
if ($actualVadSha256 -ne $vadSha256) {
    throw "FSMN-VAD model checksum mismatch: $actualVadSha256"
}

$required = @(
    (Join-Path $runtimeDir "llama-funasr-sensevoice.exe"),
    (Join-Path $modelDir "sensevoice-small-q8.gguf"),
    (Join-Path $modelDir "fsmn-vad.gguf")
)
foreach ($path in $required) {
    if (-not (Test-Path $path) -or (Get-Item $path).Length -eq 0) {
        throw "YanFlow resource is missing or empty: $path"
    }
}
Write-Host "YanFlow resources ready: $Destination"
