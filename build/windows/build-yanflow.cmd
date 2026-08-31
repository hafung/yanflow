@echo off
setlocal

set "REPO_ROOT=%~dp0..\.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"
if not defined TYPEPHP_HOME for %%I in (tpc.exe) do if not "%%~$PATH:I"=="" set "TYPEPHP_HOME=%%~dp$PATH:I"
if not defined TYPEPHP_HOME (
    echo ERROR: Set TYPEPHP_HOME to an extracted TypePHP Windows release.
    exit /b 2
)
if not defined TYPEPHP_RUNTIME_DIR if defined TYPEPHP_HOME set "TYPEPHP_RUNTIME_DIR=%TYPEPHP_HOME%"
set "DEPS_DIR=%REPO_ROOT%\build\deps\yanflow"
set "PACKAGE_DIR=%REPO_ROOT%\build\artifacts\yanflow-windows-x64"

call "%~dp0build-yanflow-worker.cmd" "%DEPS_DIR%"
if errorlevel 1 exit /b %errorlevel%

call "%~dp0build-typephp-gui.cmd" "%REPO_ROOT%\app\project.yml" "%REPO_ROOT%\build\artifacts\yanflow.exe"
if errorlevel 1 exit /b %errorlevel%

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0set-executable-icons.ps1" ^
    -Executable "%REPO_ROOT%\build\artifacts\yanflow.exe" ^
    -ApplicationIcon "%REPO_ROOT%\assets\icons\yanflow-app.ico" ^
    -FloatingIcon "%REPO_ROOT%\assets\icons\yanflow-floating.ico"
if errorlevel 1 exit /b %errorlevel%

if not exist "%DEPS_DIR%\funasr\llama-funasr-sensevoice.exe" (
    echo YanFlow resources are not installed; running setup...
    call "%~dp0setup-yanflow.cmd" "%DEPS_DIR%"
    if errorlevel 1 exit /b %errorlevel%
)

call "%~dp0stage-typephp-gui-runtime.cmd" "%REPO_ROOT%\build\artifacts\yanflow.exe" "%PACKAGE_DIR%" "%TYPEPHP_RUNTIME_DIR%"
if errorlevel 1 exit /b %errorlevel%
copy /y "%REPO_ROOT%\app\php.ini" "%PACKAGE_DIR%\php.ini" >nul
if errorlevel 1 exit /b %errorlevel%
set "PHPRC="
if exist "%PACKAGE_DIR%\funasr" rmdir /s /q "%PACKAGE_DIR%\funasr"
if exist "%PACKAGE_DIR%\models" rmdir /s /q "%PACKAGE_DIR%\models"
mkdir "%PACKAGE_DIR%\funasr"
copy /y "%DEPS_DIR%\funasr\yanflow-asr-worker.exe" "%PACKAGE_DIR%\funasr\yanflow-asr-worker.exe" >nul
if errorlevel 1 exit /b %errorlevel%
xcopy /e /i /y "%DEPS_DIR%\models" "%PACKAGE_DIR%\models" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%REPO_ROOT%\THIRD-PARTY-NOTICES.md" "%PACKAGE_DIR%\THIRD-PARTY-NOTICES.md" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%REPO_ROOT%\app\asr-worker\THIRD-PARTY-NOTICES.txt" "%PACKAGE_DIR%\THIRD-PARTY-NOTICES-worker.txt" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%REPO_ROOT%\LICENSE" "%PACKAGE_DIR%\LICENSE-YanFlow.txt" >nul
if errorlevel 1 exit /b %errorlevel%
copy /y "%REPO_ROOT%\README.md" "%PACKAGE_DIR%\README.md" >nul
if errorlevel 1 exit /b %errorlevel%

set "PHPRC=%PACKAGE_DIR%"
start "" /wait "%PACKAGE_DIR%\yanflow.exe" --smoke
set "SMOKE_EXIT=%ERRORLEVEL%"
if not "%SMOKE_EXIT%"=="0" exit /b %SMOKE_EXIT%

copy /y "%DEPS_DIR%\source\sensevoice-v0.1.9\runtime\llama.cpp\tests\sample.wav" "%PACKAGE_DIR%\yanflow-asr-smoke.wav" >nul
if errorlevel 1 exit /b %errorlevel%
start "" /wait "%PACKAGE_DIR%\yanflow.exe" --asr-smoke
set "ASR_SMOKE_EXIT=%ERRORLEVEL%"
if not "%ASR_SMOKE_EXIT%"=="0" exit /b %ASR_SMOKE_EXIT%
del /q "%PACKAGE_DIR%\yanflow-asr-smoke.wav"

echo YanFlow package: %PACKAGE_DIR%
exit /b 0
