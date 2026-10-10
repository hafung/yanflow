"""Focused native protocol/short-PCM smoke; developer Python stdlib only."""
import argparse
import array
import json
import math
from pathlib import Path
import queue
import struct
import subprocess
import tempfile
import threading
import time
import wave


def exact(stream, count):
    result = bytearray()
    while len(result) < count:
        data = stream.read(count - len(result))
        if not data:
            raise RuntimeError('Unexpected worker EOF')
        result.extend(data)
    return bytes(result)


class Worker:
    def __init__(self, repo):
        deps = repo / 'build/deps/yanflow'
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(deps / 'funasr/yanflow-asr-worker.exe'), '-m',
            str(deps / 'models/sensevoice-small-q8.gguf'), '--vad', str(deps / 'models/fsmn-vad.gguf'),
            '--threads', '8'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)
        if self.read(lambda: struct.unpack('<III', exact(self.process.stdout, 12))) != (0x31574659, 2, 0):
            raise RuntimeError('ASR v2 handshake mismatch')

    def read(self, operation):
        results = queue.Queue()
        def task():
            try:
                results.put((True, operation()))
            except Exception as exc:
                results.put((False, exc))
        thread = threading.Thread(target=task, daemon=True)
        thread.start()
        try:
            ok, value = results.get(timeout=45)
        except queue.Empty:
            self.process.kill()
            raise RuntimeError('Worker read deadline exceeded')
        if not ok:
            raise value
        return value

    def exchange(self, pcm, flags=3):
        started = time.perf_counter()
        self.process.stdin.write(struct.pack('<III', 0x31514659, len(pcm) // 2, flags) + pcm)
        self.process.stdin.flush()
        def response():
            magic, status, size, elapsed = struct.unpack('<IIIQ', exact(self.process.stdout, 20))
            if magic != 0x31524659 or status or size > 1024 * 1024:
                raise RuntimeError('Invalid response')
            text = exact(self.process.stdout, size).decode('utf-8')
            tokens = []
            if flags & 2:
                count, = struct.unpack('<I', exact(self.process.stdout, 4))
                if count > 4096:
                    raise RuntimeError('Unbounded token count')
                previous = 0
                for _ in range(count):
                    begin, end, length = struct.unpack('<III', exact(self.process.stdout, 12))
                    if begin < previous or begin > end or end > len(pcm) // 2 or length > 4096:
                        raise RuntimeError('Invalid token bounds')
                    token = exact(self.process.stdout, length).decode('utf-8').replace('\u2581', ' ')
                    tokens.append(dict(text=token, begin=begin, end=end))
                    previous = begin
            return dict(text=text, tokens=tokens, inference_ms=elapsed / 1000)
        result = self.read(response)
        result['roundtrip_ms'] = (time.perf_counter() - started) * 1000
        return result

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
        self.process.stdout.close()
        self.log.close()


def pcm_wave(path):
    with wave.open(str(path), 'rb') as source:
        assert (source.getframerate(), source.getnchannels(), source.getsampwidth()) == (16000, 1, 2)
        return source.readframes(source.getnframes())


def write_wave(path, pcm):
    with wave.open(str(path), 'wb') as destination:
        destination.setnchannels(1)
        destination.setsampwidth(2)
        destination.setframerate(16000)
        destination.writeframes(pcm)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('repo', type=Path)
    args = parser.parse_args()
    repo = args.repo.resolve()
    package = repo / 'build/artifacts/yanflow-windows-x64'
    sample = repo / 'build/deps/yanflow/source/sensevoice-v0.1.9/runtime/llama.cpp/tests/sample.wav'
    pcm = pcm_wave(sample)
    worker = Worker(repo)
    report = {'evidence': 'injected PCM and existing speech fixture; no live microphone or physical hotkey evidence'}
    try:
        baseline = worker.exchange(pcm)
        if '滨海新区有房' not in baseline['text'] or not baseline['tokens']:
            raise RuntimeError('Baseline or timed tokens missing')
        if ''.join(t['text'] for t in baseline['tokens']).strip() != baseline['text']:
            raise RuntimeError('Timed token text does not reconstruct baseline')
        report['baseline'] = baseline
        silence = bytes(12800)
        impulse = array.array('h', [0] * 6400)
        impulse[1600:1616] = array.array('h', [26000] * 16)
        for label, signal in [('silence', silence), ('one_ms_tap', impulse.tobytes())]:
            result = worker.exchange(signal, 7)
            if result['text'] or result['tokens']:
                raise RuntimeError(f'Short neural VAD admitted {label}: {result["text"]}')
            report[label] = result
        word = next(t for t in baseline['tokens'] if t['text'].strip() == '我')
        center = (word['begin'] + word['end']) // 2
        # A human-speech fragment below the previous 350 ms frontend threshold.
        start, end = max(0, center - 2400), min(len(pcm) // 2, center + 2400)
        short_pcm = pcm[start * 2:end * 2]
        padded = bytes(3200) + short_pcm
        padded += bytes(max(0, 12800 - len(padded)))
        direct = worker.exchange(padded, 7)
        report['short_word'] = dict(recorded_ms=len(short_pcm) / 32, target='我', **direct)
        if direct['text'].strip('。.!? ') != '我':
            raise RuntimeError(f'Short speech anchor missing: {direct["text"]}')
        # Keep the harder, potentially truncated final-syllable case visible.
        # This is a diagnostic, not a claim that isolated short-word CER improved.
        harder = next(t for t in reversed(baseline['tokens']) if t['text'].strip() == '房')
        center_hard = (harder['begin'] + harder['end']) // 2
        hard_pcm = pcm[max(0, center_hard - 2400) * 2:min(len(pcm)//2, center_hard + 2400) * 2]
        hard_result = worker.exchange(bytes(3200) + hard_pcm, 7)
        report['short_fragment_diagnostic'] = dict(target='房', complete_syllable_verified=False, **hard_result)
        fixture = package / 'yanflow-asr-smoke.wav'
        result_file = package / 'yanflow-short-result.txt'
        if fixture.exists() or result_file.exists():
            raise RuntimeError('Another PCM smoke appears to be running')
        try:
            write_wave(fixture, short_pcm)
            process = subprocess.run([str(package / 'yanflow.exe'), '--short-hold-smoke'], timeout=20)
            if process.returncode or not result_file.exists() or result_file.read_text(encoding='utf-8').strip('。.!? ') != '我':
                raise RuntimeError(f'Native short capture/flush pipeline failed: {process.returncode}')
            report['native_short_pipeline'] = result_file.read_text(encoding='utf-8')
        finally:
            fixture.unlink(missing_ok=True)
            result_file.unlink(missing_ok=True)
        long_result = package / 'yanflow-long-result.txt'
        chunk_file = package / 'yanflow-long-chunks.tsv'
        repeated = array.array('h')
        repeated.frombytes((pcm + bytes(12800)) * 3)
        report['long_recordings'] = []
        for mode in ['quiet_boundaries', 'continuous_background', 'hands_free_continuous']:
            signal = array.array('h', repeated)
            expected_repetitions = 3
            if mode == 'hands_free_continuous':
                # Preserve the sounds but remove the sample's long internal
                # question pause, producing a continuous-speech fixture.
                first_i = next(t for t in baseline['tokens'] if t['text'] == '我')
                question = next(t for t in baseline['tokens'] if t['text'] == '问')
                second_i = next(t for t in baseline['tokens'] if t['text'] == '我' and t['begin'] > question['end'])
                final_word = next(t for t in reversed(baseline['tokens']) if t['text'] == '房')
                begin = max(0, first_i['begin'] - 3200)
                pause_begin, pause_end = question['end'] + 2400, second_i['begin'] - 3200
                end = min(len(pcm)//2, final_word['end'] + 4800)
                if not begin < pause_begin < pause_end < end:
                    raise RuntimeError('Continuous fixture pause bounds invalid')
                continuous = pcm[begin*2:pause_begin*2] + pcm[pause_end*2:end*2]
                expected_repetitions = 5
                signal = array.array('h')
                signal.frombytes(continuous * expected_repetitions)
            if mode != 'quiet_boundaries':
                for i, sample in enumerate(signal):
                    signal[i] = max(-32768, min(32767, sample + round(600 * math.sin(math.pi * i / 2))))
            if fixture.exists() or long_result.exists() or chunk_file.exists():
                raise RuntimeError('Another long PCM smoke appears to be running')
            try:
                write_wave(fixture, signal.tobytes())
                started = time.perf_counter()
                option = '--long-stream-smoke' if mode == 'hands_free_continuous' else '--long-hold-smoke'
                process = subprocess.run([str(package / 'yanflow.exe'), option], timeout=55 if mode == 'hands_free_continuous' else 30)
                if process.returncode or not long_result.exists() or not chunk_file.exists():
                    raise RuntimeError(f'Long held pipeline failed: {process.returncode}')
                text = long_result.read_text(encoding='utf-8')
                chunks = [list(map(int, line.split('\t'))) for line in chunk_file.read_text(encoding='utf-8').splitlines()[1:]]
                for begin, end, owned_begin, owned_end in chunks:
                    if end - begin > 128000 or not begin <= owned_begin < owned_end <= end:
                        raise RuntimeError('Long inference exceeded bounded ownership window')
                result = dict(mode=mode, text=text, chunks=chunks,
                              audio_ms=len(signal)/16, expected_repetitions=expected_repetitions,
                              process_ms=(time.perf_counter() - started) * 1000,
                              repeated_phrase_count=text.count('滨海新区有房'))
                report['long_recordings'].append(result)
                if len(chunks) < 3 or result['repeated_phrase_count'] != expected_repetitions or '问问' in text:
                    raise RuntimeError(f'Long held output lost or duplicated the repeated phrase: {text}')
                punctuation = str.maketrans('', '', '。.,，!?！？ ')
                if text.translate(punctuation) != baseline['text'].translate(punctuation) * expected_repetitions:
                    raise RuntimeError(f'Long recording changed speech words at a boundary: {text}')
                if mode != 'quiet_boundaries' and not any(begin < own for begin, _, own, _ in chunks):
                    raise RuntimeError('Forced-boundary overlap was not exercised')
            finally:
                fixture.unlink(missing_ok=True)
                long_result.unlink(missing_ok=True)
                chunk_file.unlink(missing_ok=True)
        # Unknown flags must fail explicitly rather than desynchronize the pipe.
        worker.process.stdin.write(struct.pack('<III', 0x31514659, 0, 64))
        worker.process.stdin.flush()
        worker.process.wait(timeout=5)
        if worker.process.returncode != 5:
            raise RuntimeError('Unknown request flags did not exit with protocol error')
        report['unknown_flags_exit'] = 5
        print('PASS native ASR v2 timing, short speech, quiet/forced long boundaries, repeated phrase retention, silence/tap rejection, invalid flags')
    finally:
        worker.close()
        (repo / 'build/artifacts/accuracy-worker-smoke.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
