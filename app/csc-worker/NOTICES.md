Optional Chinese spelling correction bundle

- MacBERT4CSC base Chinese by Xu Ming / shibing624, Apache-2.0.
  Original model: https://huggingface.co/shibing624/macbert4csc-base-chinese
  INT8 ONNX conversion: Xenova/macbert4csc-base-chinese, revision
  7ebfe81cf502576e93c844b95354840b2ecb5c28. README-model.md retains the conversion card.
  https://huggingface.co/Xenova/macbert4csc-base-chinese
- ONNX Runtime 1.20.1 by Microsoft, MIT. LICENSE-onnxruntime.txt and
  NOTICE-onnxruntime.txt retain upstream notices.
- Microsoft Visual C++ runtime DLLs are copied from the build toolchain's
  redistributable x64 CRT directory under its redistributable terms.
  The bundle's SHA256SUMS records these exact app-local files.

YanFlow does not modify model weights. Confidence gates are application policy,
not calibrated correctness probabilities. This optional model is disabled by default.
