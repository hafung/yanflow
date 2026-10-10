param(
    [string]$DependencyRoot = "",
    [int]$Iterations = 20
)

$ErrorActionPreference = "Stop"
if ($Iterations -lt 0 -or $Iterations -eq 1) { throw "Iterations must be 0 or at least 2" }
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($DependencyRoot)) {
    $DependencyRoot = Join-Path $repoRoot "build\deps\yanflow"
}
$worker = Join-Path $DependencyRoot "funasr\yanflow-asr-worker.exe"
$model = Join-Path $DependencyRoot "models\sensevoice-small-q8.gguf"
$vad = Join-Path $DependencyRoot "models\fsmn-vad.gguf"
$sourceRoot = Join-Path $DependencyRoot "source\sensevoice-v0.1.9"
$wave = Join-Path $sourceRoot "runtime\llama.cpp\tests\sample.wav"
foreach ($path in @($worker, $model, $vad, $wave)) {
    if (-not (Test-Path $path)) { throw "Worker smoke dependency missing: $path" }
}

function Read-Pcm16Wave([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne "RIFF") { throw "Not RIFF" }
        [void]$reader.ReadUInt32()
        if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne "WAVE") { throw "Not WAVE" }
        $formatOk = $false
        while ($stream.Position -lt $stream.Length) {
            $chunk = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
            $size = $reader.ReadUInt32()
            if ($chunk -eq "fmt ") {
                $format = $reader.ReadUInt16()
                $channels = $reader.ReadUInt16()
                $rate = $reader.ReadUInt32()
                [void]$reader.ReadUInt32(); [void]$reader.ReadUInt16()
                $bits = $reader.ReadUInt16()
                if ($size -gt 16) { [void]$reader.ReadBytes([int]$size - 16) }
                $formatOk = $format -eq 1 -and $channels -eq 1 -and $rate -eq 16000 -and $bits -eq 16
            } elseif ($chunk -eq "data") {
                if (-not $formatOk) { throw "Expected PCM16 mono 16 kHz" }
                return $reader.ReadBytes([int]$size)
            } else {
                [void]$reader.ReadBytes([int]$size)
            }
            if (($size % 2) -ne 0) { [void]$reader.ReadByte() }
        }
        throw "WAVE data chunk missing"
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

$startInfo = [Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $worker
$startInfo.Arguments = "-m `"$model`" --vad `"$vad`" --threads 8"
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.RedirectStandardInput = $true
$startInfo.RedirectStandardOutput = $true
$startInfo.RedirectStandardError = $true
$process = [Diagnostics.Process]::new()
$process.StartInfo = $startInfo
$stopwatch = [Diagnostics.Stopwatch]::StartNew()
if (-not $process.Start()) { throw "Could not start worker" }
$input = [IO.BinaryWriter]::new($process.StandardInput.BaseStream)
$output = [IO.BinaryReader]::new($process.StandardOutput.BaseStream)
$englishWave = Join-Path ([IO.Path]::GetTempPath()) "yanflow-english-$PID.wav"
try {
    $readyMagic = $output.ReadUInt32()
    $version = $output.ReadUInt32()
    $readyStatus = $output.ReadUInt32()
    $loadMilliseconds = $stopwatch.ElapsedMilliseconds
    if ($readyMagic -ne 0x31574659 -or $version -ne 2 -or $readyStatus -ne 0) {
        throw "Invalid worker handshake"
    }
    if ($Iterations -eq 0) {
        $input.Dispose()
        if (-not $process.WaitForExit(5000)) { throw "Worker did not exit after startup handshake" }
        if ($process.ExitCode -ne 0) { throw "Worker exited with $($process.ExitCode)" }
        Write-Host "PASS yanflow-worker-startup load_ms=$loadMilliseconds protocol_version=$version"
        return
    }
    $pcm = Read-Pcm16Wave $wave
    $sampleCount = [uint32]($pcm.Length / 2)
    $expected = -join @([char]0x6ee8, [char]0x6d77, [char]0x65b0, [char]0x533a, [char]0x6709, [char]0x623f)
    $elapsed = @()
    $workingSets = @()
    $lastText = ""
    for ($iteration = 0; $iteration -lt $Iterations; $iteration++) {
        $input.Write([uint32]0x31514659)
        $input.Write($sampleCount)
        $input.Write([uint32]1)
        $input.BaseStream.Write($pcm, 0, $pcm.Length)
        $input.Flush()
        $magic = $output.ReadUInt32()
        $status = $output.ReadUInt32()
        $bytes = $output.ReadUInt32()
        $elapsedMicroseconds = $output.ReadUInt64()
        $text = [Text.Encoding]::UTF8.GetString($output.ReadBytes([int]$bytes))
        if ($magic -ne 0x31524659 -or $status -ne 0) { throw "Worker response failed" }
        if (-not $text.Contains($expected)) { throw "Unexpected transcript: $text" }
        $lastText = $text
        $elapsed += [math]::Round($elapsedMicroseconds / 1000.0, 1)
        $process.Refresh()
        $workingSets += $process.WorkingSet64
    }

    $silencePcm = [byte[]]::new(16000 * 2)
    $clicksPcm = [byte[]]::new(16000 * 2)
    for ($offset = 0; $offset -lt $clicksPcm.Length; $offset += 8000) {
        $clicksPcm[$offset] = 0xff
        $clicksPcm[$offset + 1] = 0x7f
    }
    foreach ($noise in @($silencePcm, $clicksPcm)) {
        $input.Write([uint32]0x31514659)
        $input.Write([uint32]($noise.Length / 2))
        $input.Write([uint32]1)
        $input.BaseStream.Write($noise, 0, $noise.Length)
        $input.Flush()
        $noiseMagic = $output.ReadUInt32()
        $noiseStatus = $output.ReadUInt32()
        $noiseBytes = $output.ReadUInt32()
        [void]$output.ReadUInt64()
        $noiseText = [Text.Encoding]::UTF8.GetString($output.ReadBytes([int]$noiseBytes))
        if ($noiseMagic -ne 0x31524659 -or $noiseStatus -ne 0 -or $noiseText.Length -ne 0) {
            throw "VAD emitted text for non-speech input: $noiseText"
        }
    }

    $voice = New-Object -ComObject SAPI.SpVoice
    $audioStream = New-Object -ComObject SAPI.SpFileStream
    $audioFormat = New-Object -ComObject SAPI.SpAudioFormat
    try {
        # SAFT16kHz16BitMono. The worker protocol always receives 16 kHz PCM16.
        $audioFormat.Type = 18
        $audioStream.Format = $audioFormat
        $audioStream.Open($englishWave, 3, $false)
        $voice.AudioOutputStream = $audioStream
        [void]$voice.Speak("The quick brown fox jumps over the lazy dog near the river bank.")
        $audioStream.Close()
    } finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($audioFormat)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($audioStream)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($voice)
    }
    $englishPcm = Read-Pcm16Wave $englishWave
    $input.Write([uint32]0x31514659)
    $input.Write([uint32]($englishPcm.Length / 2))
    $input.Write([uint32]0)
    $input.BaseStream.Write($englishPcm, 0, $englishPcm.Length)
    $input.Flush()
    $englishMagic = $output.ReadUInt32()
    $englishStatus = $output.ReadUInt32()
    $englishBytes = $output.ReadUInt32()
    [void]$output.ReadUInt64()
    $englishText = [Text.Encoding]::UTF8.GetString($output.ReadBytes([int]$englishBytes))
    $englishWords = [regex]::Matches($englishText, "[A-Za-z]+").Count
    if ($englishMagic -ne 0x31524659 -or $englishStatus -ne 0 -or $englishWords -lt 6 -or
        -not $englishText.ToLowerInvariant().Contains("quick") -or
        -not $englishText.ToLowerInvariant().Contains("river")) {
        throw "English transcript was truncated or incorrect: $englishText"
    }
    $input.Dispose()
    if (-not $process.WaitForExit(5000)) { throw "Worker did not exit after stdin closed" }
    if ($process.ExitCode -ne 0) { throw "Worker exited with $($process.ExitCode)" }
    $sorted = @($elapsed | Sort-Object)
    $p95Index = [math]::Min($sorted.Count - 1, [math]::Ceiling($sorted.Count * 0.95) - 1)
    $average = [math]::Round(($elapsed | Measure-Object -Average).Average, 1)
    $memoryDriftMb = [math]::Round(($workingSets[-1] - $workingSets[0]) / 1MB, 1)
    if ($memoryDriftMb -gt 64) { throw "Worker working-set drift too high: $memoryDriftMb MB" }
    Write-Host "PASS yanflow-worker requests=$Iterations vad_non_speech=silence,clicks load_ms=$loadMilliseconds infer_avg_ms=$average infer_p95_ms=$($sorted[$p95Index]) working_set_drift_mb=$memoryDriftMb text=$lastText english_words=$englishWords english=$englishText"
} finally {
    Remove-Item -Force -ErrorAction SilentlyContinue $englishWave
    $output.Dispose()
    if (-not $process.HasExited) { $process.Kill() }
    $process.Dispose()
}
