"""Real-time PCM replay through endpoint, preview, final ASR and a native Edit.

Use --wave/--output under local/asr-eval for private inputs/results. Neither
audio nor reference text is embedded here. This does not exercise a live mic.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import tempfile
import time
import threading
import wave


def read_wave(path):
    with wave.open(str(path), 'rb') as source:
        assert (source.getnchannels(), source.getsampwidth(), source.getframerate()) == (1, 2, 16000)
        return source.readframes(source.getnframes())


def write_wave(path, pcm):
    with wave.open(str(path), 'wb') as target:
        target.setparams((1, 2, 16000, 0, 'NONE', 'not compressed'))
        target.writeframes(pcm)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('repo', type=Path)
    parser.add_argument('--wave', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--repeats', type=int, default=1)
    args = parser.parse_args()
    repo = args.repo.resolve()
    package = repo/'build/artifacts/yanflow-windows-x64'
    sample = repo/'build/deps/yanflow/source/sensevoice-v0.1.9/runtime/llama.cpp/tests/sample.wav'
    short = read_wave(sample)
    long = read_wave(args.wave) if args.wave else short*4
    assert 8 < len(long)/32000 <= 60
    assert args.repeats > 0
    rows = []
    artifacts = repo/'build/artifacts'
    with tempfile.TemporaryDirectory(prefix='prepared-asr-', dir=artifacts) as temp:
        root = Path(temp)
        for relative in ('yanflow.exe', 'funasr/yanflow-asr-worker.exe',
                         'models/sensevoice-small-q8.gguf', 'models/fsmn-vad.gguf'):
            target = root/relative
            target.parent.mkdir(parents=True, exist_ok=True)
            try:
                os.link(package/relative, target)
            except OSError:
                shutil.copy2(package/relative, target)
        for name, pcm in [('short', short), ('long', long)]:
            write_wave(root/'yanflow-asr-smoke.wav', pcm)
            for repeat in range(args.repeats):
                # Alternate order to reduce thermal/background-load bias.
                modes = [False, True] if repeat % 2 == 0 else [True, False]
                for enabled in modes:
                    result_path = root/'yanflow-preview-result.txt'
                    comparison = root/'yanflow-prepared-comparison.jsonl'
                    for output in (result_path, comparison):
                        output.unlink(missing_ok=True)
                    option = '--prepared-asr-smoke' if enabled else '--unprepared-asr-smoke'
                    start = time.perf_counter()
                    # Match existing Windows GUI smokes: ShellExecute through
                    # Start-Process gives the fixture foreground activation rights.
                    quote = lambda value: "'"+str(value).replace("'", "''")+"'"
                    launch = (f'$p=Start-Process -FilePath {quote(root/"yanflow.exe")} '
                              f'-ArgumentList {quote(option)} -WorkingDirectory {quote(root)} -PassThru; '
                              f'if(-not $p.WaitForExit({int((len(pcm)/32000+20)*1000)})) '
                              '{$p.Kill(); exit 125}; exit $p.ExitCode')
                    child = subprocess.Popen(['powershell.exe', '-NoProfile', '-Command', launch], cwd=root,
                                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    foreground_events = []
                    monitoring = threading.Event()
                    def monitor():
                        user = ctypes.windll.user32
                        user.GetForegroundWindow.restype = ctypes.c_void_p
                        user.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
                        while not monitoring.wait(.1):
                            name = ctypes.create_unicode_buffer(256)
                            user.GetClassNameW(user.GetForegroundWindow(), name, 256)
                            if not foreground_events or foreground_events[-1][1] != name.value:
                                foreground_events.append((round(time.perf_counter()-start, 2), name.value))
                    observer = threading.Thread(target=monitor)
                    observer.start()
                    try:
                        code = child.wait(timeout=len(pcm)/32000+25)
                    except subprocess.TimeoutExpired:
                        child.kill(); child.wait()
                        raise AssertionError(f'{name} {option} timed out')
                    finally:
                        monitoring.set(); observer.join()
                    details = result_path.read_text(encoding='utf-8-sig') if result_path.exists() else ''
                    assert code == 0, (name, option, code, details, foreground_events)
                    metrics = {k:int(v) for k,v in re.findall(r'(release_to_delivery_us|prepared_windows|reused_windows|worker_cpu_us)=(\d+)', details)}
                    assert len(metrics) == 4, details
                    outputs = [json.loads(line) for line in comparison.read_text(encoding='utf-8-sig').splitlines()]
                    assert len(outputs) == 1, outputs
                    output = outputs[0]
                    assert output['delivery_status'] == 'verified'
                    assert output['observed_inserted'] == output['final']
                    assert 'premature=0' in details and 'injected=1' in details and 'visible=1' in details
                    if not enabled or name == 'short':
                        assert metrics['prepared_windows'] == metrics['reused_windows'] == 0, metrics
                    else:
                        assert metrics['reused_windows'] > 0, metrics
                    row = dict(case=name, repeat=repeat, preparation=enabled,
                               duration_s=len(pcm)/32000, wall_s=time.perf_counter()-start,
                               raw=output['raw'], final=output['final'], **metrics)
                    rows.append(row)
                    print(json.dumps({k:v for k,v in row.items() if k not in ('raw','final')}, ensure_ascii=True), flush=True)
                    if args.output:
                        args.output.parent.mkdir(parents=True, exist_ok=True)
                        args.output.write_text(json.dumps(dict(runs=rows),ensure_ascii=False,indent=2),encoding='utf-8')
            case = [r for r in rows if r['case']==name]
            assert len({r['raw'] for r in case}) == 1, f'{name}: raw transcript changed'
            assert len({r['final'] for r in case}) == 1, f'{name}: corrected transcript changed'
    summaries = []
    for name in ('short', 'long'):
        before = [r for r in rows if r['case']==name and not r['preparation']]
        after = [r for r in rows if r['case']==name and r['preparation']]
        old = statistics.median(r['release_to_delivery_us'] for r in before)/1000
        new = statistics.median(r['release_to_delivery_us'] for r in after)/1000
        cpu_old = statistics.median(r['worker_cpu_us'] for r in before)/1e6
        cpu_new = statistics.median(r['worker_cpu_us'] for r in after)/1e6
        summary = dict(case=name, before_ms=old, after_ms=new, improvement=1-new/old,
                       worker_cpu_before_s=cpu_old, worker_cpu_after_s=cpu_new,
                       identical_raw=True, identical_final=True, verified_native_edit=True)
        summaries.append(summary)
        print(json.dumps(summary), flush=True)
    if args.output:
        args.output.write_text(json.dumps(dict(runs=rows,summary=summaries,
            evidence='Real-time file PCM replay through application endpoint/inference/preview and native Edit verification. Synthetic release; no live microphone or third-party application evidence.'),ensure_ascii=False,indent=2),encoding='utf-8')
    assert summaries[1]['after_ms'] < summaries[1]['before_ms']*.75, 'Long-utterance latency did not improve enough'
    assert summaries[0]['after_ms'] <= summaries[0]['before_ms']+100, 'Short-utterance latency regressed'


if __name__ == '__main__':
    main()
