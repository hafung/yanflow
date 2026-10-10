"""Paired raw-ASR/corrected CER evaluation. No third-party Python dependencies.

Use hand-transcribed expected text; synthetic examples establish guards only.
By default scores saved raw/final pairs. --exe replays raw through the current
portable correction pipeline, independent of microphone/audio inference.
"""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def align(reference, hypothesis):
    matrix = [list(range(len(hypothesis) + 1))]
    for i, char in enumerate(reference, 1):
        row = [i]
        for j, other in enumerate(hypothesis, 1):
            row.append(min(matrix[i - 1][j] + 1, row[j - 1] + 1,
                           matrix[i - 1][j - 1] + (char != other)))
        matrix.append(row)
    correct_positions = set()
    i, j = len(reference), len(hypothesis)
    while i or j:
        if i and j and matrix[i][j] == matrix[i - 1][j - 1] + (reference[i - 1] != hypothesis[j - 1]):
            if reference[i - 1] == hypothesis[j - 1]:
                correct_positions.add(i - 1)
            i, j = i - 1, j - 1
        elif i and matrix[i][j] == matrix[i - 1][j] + 1:
            i -= 1
        else:
            j -= 1
    return matrix[-1][-1], correct_positions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("corpus", type=Path, help="UTF-8 JSONL: raw, final, expected")
    parser.add_argument("--exe", type=Path, help="replay with yanflow.exe --correct-text")
    parser.add_argument("--macbert", action="store_true")
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.macbert and not args.exe:
        parser.error("--macbert requires --exe")
    rows = []
    with tempfile.TemporaryDirectory(prefix="yanflow correction ") as temporary:
        for line_number, line in enumerate(args.corpus.read_text(encoding="utf-8-sig").splitlines(), 1):
            if not line.strip():
                continue
            item = json.loads(line)
            raw, expected = item.get("raw"), item.get("expected")
            if not isinstance(raw, str) or not isinstance(expected, str) or not expected:
                raise ValueError(f"Line {line_number}: raw and nonempty hand-transcribed expected are required")
            if args.exe:
                source, destination = Path(temporary) / "raw.txt", Path(temporary) / "final.txt"
                source.write_text(raw, encoding="utf-8")
                command = [str(args.exe.resolve()), "--correct-text", str(source), str(destination)]
                if args.macbert:
                    command.append("--macbert")
                subprocess.run(command, check=True, timeout=45)
                final = destination.read_text(encoding="utf-8")
            else:
                final = item.get("final")
                if not isinstance(final, str):
                    raise ValueError(f"Line {line_number}: final required without --exe")
            raw_errors, raw_correct = align(expected, raw)
            final_errors, final_correct = align(expected, final)
            rows.append(dict(id=item.get("id", line_number), raw=raw, final=final, expected=expected,
                             reference_characters=len(expected), raw_errors=raw_errors, final_errors=final_errors,
                             damaged_correct_characters=len(raw_correct - final_correct),
                             clean_changed=raw == expected and final != expected))
    if not rows:
        raise ValueError("Empty corpus")
    characters = sum(row["reference_characters"] for row in rows)
    raw_errors = sum(row["raw_errors"] for row in rows)
    final_errors = sum(row["final_errors"] for row in rows)
    damaged = sum(row["damaged_correct_characters"] for row in rows)
    clean_changed = sum(row["clean_changed"] for row in rows)
    report = dict(samples=len(rows), reference_characters=characters,
                  raw_errors=raw_errors, final_errors=final_errors,
                  raw_cer=raw_errors / characters, final_cer=final_errors / characters,
                  damaged_correct_characters=damaged, clean_sentences_changed=clean_changed,
                  regressed_sentences=sum(row["final_errors"] > row["raw_errors"] for row in rows),
                  passes=final_errors < raw_errors and damaged == 0 and clean_changed == 0,
                  note="CER includes case, punctuation and spaces. Alignment damage is an estimate; review paired text. Synthetic data is not live-ASR accuracy evidence.",
                  rows=rows)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key != "rows"}, ensure_ascii=False))
    return 0 if report["passes"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
