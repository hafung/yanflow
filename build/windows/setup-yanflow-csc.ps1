$ErrorActionPreference = "Stop"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$deps = Join-Path $repo "build\deps\yanflow"
$downloads = Join-Path $deps "downloads"
$staging = Join-Path $deps "csc.partial"
New-Item -ItemType Directory -Force $downloads, $staging | Out-Null
Get-ChildItem -LiteralPath $staging -File | Remove-Item -Force

function Get-Pinned([string]$Url, [string]$Name, [string]$Sha256) {
    $path = Join-Path $downloads $Name
    if (-not (Test-Path -LiteralPath $path)) {
        & curl.exe --fail --location --retry 5 --retry-delay 2 --output "$path.partial" $Url
        if ($LASTEXITCODE -ne 0) { throw "CSC resource download failed: $Name" }
        Move-Item -Force -LiteralPath "$path.partial" -Destination $path
    }
    $actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) { throw "CSC resource checksum mismatch: $Name ($actual)" }
    return $path
}
$ortZip = Get-Pinned "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-win-x64-1.20.1.zip" `
    "onnxruntime-win-x64-1.20.1.zip" "78d447051e48bd2e1e778bba378bec4ece11191c9e538cf7b2c4a4565e8f5581"
$revision = "7ebfe81cf502576e93c844b95354840b2ecb5c28"
$base = "https://huggingface.co/Xenova/macbert4csc-base-chinese/resolve/$revision"
$model = Get-Pinned "$base/onnx/model_int8.onnx" "macbert4csc-int8-$revision.onnx" "11fd06011b1381bcb2f5ac1cd9c208d502ce652876c79c2903bcd786e0bfb97b"
$vocab = Get-Pinned "$base/vocab.txt" "macbert4csc-vocab-$revision.txt" "45bbac6b341c319adc98a532532882e91a9cefc0329aa57bac9ae761c27b291c"
$card = Get-Pinned "$base/README.md" "macbert4csc-card-$revision.md" "15fc16bc3a465e8262e7fc1b8bac027f8c169a7470bec2e2fad1ef35e0fa87be"

$ortSource = Join-Path $deps "source\onnxruntime-1.20.1"
# Re-extract verified headers/runtime; do not trust a stale extraction.
if (Test-Path -LiteralPath $ortSource) { Remove-Item -Recurse -Force -LiteralPath $ortSource }
Expand-Archive -LiteralPath $ortZip -DestinationPath $ortSource
$ort = Join-Path $ortSource "onnxruntime-win-x64-1.20.1"
$cmake = "cmake.exe"
if (-not (Get-Command $cmake -ErrorAction SilentlyContinue)) {
    $cmake = Join-Path $env:VS_BUILD_TOOLS "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
$build = Join-Path $repo "build\native\yanflow-csc"
& $cmake -S (Join-Path $repo "app\csc-worker") -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DONNXRUNTIME_DIR=$ort"
if ($LASTEXITCODE -ne 0) { throw "CSC configure failed" }
& $cmake --build $build --target yanflow-csc-worker
if ($LASTEXITCODE -ne 0) { throw "CSC build failed" }

# Ship the DLL's VC runtime dependencies app-locally; users need no installer.
foreach ($name in @("msvcp140.dll", "msvcp140_1.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
    $runtime = Get-ChildItem -LiteralPath $env:VCToolsRedistDir -Recurse -Filter $name |
        Where-Object { $_.FullName -match "[\\/]x64[\\/].*Microsoft.VC.*CRT" } | Select-Object -First 1
    if (-not $runtime) { throw "Missing app-local VC runtime: $name" }
    Copy-Item -LiteralPath $runtime.FullName -Destination (Join-Path $staging $name) -Force
}
Copy-Item -LiteralPath (Join-Path $build "yanflow-csc-worker.exe") -Destination $staging -Force
Copy-Item -LiteralPath (Join-Path $ort "lib\onnxruntime.dll") -Destination (Join-Path $staging "yanflow-onnxruntime.dll") -Force
Copy-Item -LiteralPath (Join-Path $ort "LICENSE") -Destination (Join-Path $staging "LICENSE-onnxruntime.txt") -Force
Copy-Item -LiteralPath (Join-Path $ort "ThirdPartyNotices.txt") -Destination (Join-Path $staging "NOTICE-onnxruntime.txt") -Force
Copy-Item -LiteralPath $model -Destination (Join-Path $staging "model.onnx") -Force
Copy-Item -LiteralPath $vocab -Destination (Join-Path $staging "vocab.txt") -Force
Copy-Item -LiteralPath $card -Destination (Join-Path $staging "README-model.md") -Force
Copy-Item -LiteralPath (Join-Path $repo "LICENSES\Apache-2.0.txt") -Destination (Join-Path $staging "LICENSE-model.txt") -Force
Copy-Item -LiteralPath (Join-Path $repo "app\csc-worker\NOTICES.md") -Destination $staging -Force
Push-Location $staging
try {
    & .\yanflow-csc-worker.exe --smoke
    if ($LASTEXITCODE -ne 0) { throw "CSC model/tokenizer smoke failed: $LASTEXITCODE" }
} finally { Pop-Location }
$hashes = Get-ChildItem -LiteralPath $staging -File | Where-Object { $_.Name -ne "SHA256SUMS" } | Sort-Object Name | ForEach-Object {
    (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant() + "  " + $_.Name
}
[IO.File]::WriteAllLines((Join-Path $staging "SHA256SUMS"), $hashes, [Text.UTF8Encoding]::new($false))
$destination = Join-Path $deps "csc"
if (Test-Path -LiteralPath $destination) { Remove-Item -Recurse -Force -LiteralPath $destination }
Move-Item -LiteralPath $staging -Destination $destination
Write-Host "CSC bundle verified. Standard builds stage it automatically; enable MacBERT from the orb menu."
