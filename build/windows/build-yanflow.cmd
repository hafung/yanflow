@echo off
setlocal

set "REPO_ROOT=%~dp0..\.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"
set "DEPS_DIR=%REPO_ROOT%\build\deps\yanflow"
set "PACKAGE_DIR=%REPO_ROOT%\build\artifacts\yanflow-windows-x64"

call "%~dp0build-yanflow-worker.cmd" "%DEPS_DIR%"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0build-yanflow-gui.cmd"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0setup-yanflow.cmd" "%DEPS_DIR%"
if errorlevel 1 exit /b %errorlevel%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0stage-yanflow-package.ps1"
if errorlevel 1 exit /b %errorlevel%

start "" /wait "%PACKAGE_DIR%\yanflow.exe" --smoke
set "SMOKE_EXIT=%ERRORLEVEL%"
if not "%SMOKE_EXIT%"=="0" exit /b %SMOKE_EXIT%

if not defined YANFLOW_SKIP_ASR_SMOKE (
    copy /y "%DEPS_DIR%\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav" "%PACKAGE_DIR%\yanflow-asr-smoke.wav" >nul
    if errorlevel 1 exit /b %errorlevel%
    start "" /wait "%PACKAGE_DIR%\yanflow.exe" --asr-smoke
    if errorlevel 1 exit /b 24
    del /q "%PACKAGE_DIR%\yanflow-asr-smoke.wav"
) else (
    echo Skipping inference smoke; YANFLOW_SKIP_ASR_SMOKE is set.
)

echo YanFlow package: %PACKAGE_DIR%
exit /b 0
