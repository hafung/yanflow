@echo off
setlocal
if not defined VS_BUILD_TOOLS (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_BUILD_TOOLS=%%I"
)
if not defined VS_BUILD_TOOLS exit /b 2
call "%VS_BUILD_TOOLS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-yanflow-csc.ps1"
exit /b %errorlevel%
