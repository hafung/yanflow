# Third-party notices

YanFlow source is distributed under GPL-3.0-only. Release archives also contain separately
licensed components and model weights:

- The initial YanFlow implementation was developed in TypePHP Native Core under MIT; its retained
  license notice is in `LICENSES/MIT-native-core.txt`.
- The current native C++ release does not bundle TypePHP or PHP. The earlier implementation is
  preserved on the `history/typephp-2026-10-08` branch.
- FunASR llama.cpp runtime and YanFlow's persistent-worker foundation: MIT; details and retained
  notices are in `app/asr-worker/THIRD-PARTY-NOTICES.txt`.
- SenseVoiceSmall GGUF and FSMN-VAD GGUF: distributed by FunAudioLLM. Consult their model cards and
  terms before redistribution or commercial use.
- llama.cpp / ggml: MIT, pinned by commit in `build/windows/build-yanflow-worker.ps1`.

The build scripts pin source archives, runtime archives, and model revisions. Downloaded runtime
and model files are not committed to this repository.
