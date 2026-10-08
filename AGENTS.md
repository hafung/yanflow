# YanFlow contributor guide

YanFlow is a Windows x64, local-first voice layer built with C++17 and native Win32 APIs.

- Keep application sources explicit in `app/CMakeLists.txt`; do not reintroduce PHP or PHPX dependencies.
- Use the native Windows GUI entry point and keep recognition in the independent C++ worker.
- Audio callbacks must not block or allocate unpredictably. Keep inference off the capture thread.
- Do not weaken pinned revisions or SHA-256 checks for downloaded tools, sources, runtimes, or models.
- Preserve the portable-package contract: extracting the Release ZIP and launching `yanflow.exe`
  must be sufficient.
- Run `build\windows\build-yanflow.cmd` after C++, worker, packaging, or runtime changes. Add the
  smallest relevant focused smoke for error-path fixes.
- Do not claim live microphone, app-injection, platform, or longevity evidence that was not run.
