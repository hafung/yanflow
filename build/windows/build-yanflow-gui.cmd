@echo off
setlocal
set "REPO_ROOT=%~dp0..\.."
for %%I in ("%REPO_ROOT%") do set "REPO_ROOT=%%~fI"
if not defined VS_BUILD_TOOLS (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_BUILD_TOOLS=%%I"
)
if not defined VS_BUILD_TOOLS (
    echo ERROR: Visual Studio C++ Build Tools not found. Set VS_BUILD_TOOLS.
    exit /b 2
)
call "%VS_BUILD_TOOLS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
set "CMAKE_EXE=cmake.exe"
where cmake.exe >nul 2>nul
if errorlevel 1 set "CMAKE_EXE=%VS_BUILD_TOOLS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

"%CMAKE_EXE%" -S "%REPO_ROOT%\app" -B "%REPO_ROOT%\build\native\yanflow" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%
"%CMAKE_EXE%" --build "%REPO_ROOT%\build\native\yanflow" --target yanflow
if errorlevel 1 exit /b %errorlevel%
if not exist "%REPO_ROOT%\build\artifacts" mkdir "%REPO_ROOT%\build\artifacts"
copy /y "%REPO_ROOT%\build\native\yanflow\yanflow.exe" "%REPO_ROOT%\build\artifacts\yanflow.exe" >nul
exit /b %errorlevel%
