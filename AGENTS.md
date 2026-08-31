# YanFlow contributor guide

YanFlow is a Windows x64, local-first voice layer built with TypePHP AOT and native C++ adapters.

- Keep AOT PHP sources explicit in `app/project.yml`; do not use runtime discovery, reflection,
  dynamic code generation, variable variables, `extract()`, or `eval()`.
- C++ adapters keep declarations in `.stub.php` and implementations in lowercase `php_*` functions
  using PHPX types.
- Audio callbacks must not block or allocate unpredictably. Keep inference off the capture thread.
- Do not weaken pinned revisions or SHA-256 checks for downloaded tools, sources, runtimes, or models.
- Preserve the portable-package contract: extracting the Release ZIP and launching `yanflow.exe`
  must be sufficient.
- Run `build\windows\build-yanflow.cmd` after PHP, C++, worker, packaging, or runtime changes. Add the
  smallest relevant focused smoke for error-path fixes.
- Do not claim live microphone, app-injection, platform, or longevity evidence that was not run.
