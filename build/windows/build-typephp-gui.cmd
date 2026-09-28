@echo off
setlocal

if "%~2"=="" (
    echo Usage: build-typephp-gui.cmd PROJECT_YML OUTPUT_EXE
    exit /b 2
)

for %%I in ("%~1") do set "PROJECT_YML=%%~fI"
for %%I in ("%~2") do (
    set "OUTPUT_EXE=%%~fI"
    set "OUTPUT_DIR=%%~dpI"
)
if not defined TYPEPHP_HOME for %%I in (tpc.exe) do if not "%%~$PATH:I"=="" set "TYPEPHP_HOME=%%~dp$PATH:I"
if not defined TYPEPHP_HOME (
    echo ERROR: Set TYPEPHP_HOME to an extracted TypePHP Windows release.
    exit /b 2
)
if not defined PHP_HOME set "PHP_HOME=%TYPEPHP_HOME%"
if not defined PHPX_HOME set "PHPX_HOME=%TYPEPHP_HOME%\phpx"
rem Keep PHP and PHPX paths slash-normalized for TypePHP's Windows path checks.
set "PHP_HOME=%PHP_HOME:\=/%"
set "PHPX_HOME=%PHPX_HOME:\=/%"
set "PATH=%TYPEPHP_HOME%;%PATH%"
set "PHPRC=%TYPEPHP_HOME%"
if not defined VS_BUILD_TOOLS (
    for /f "usebackq tokens=*" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_BUILD_TOOLS=%%I"
)
if not defined VS_BUILD_TOOLS (
    echo ERROR: Visual Studio C++ Build Tools not found. Set VS_BUILD_TOOLS.
    exit /b 2
)

if not exist "%PROJECT_YML%" (
    echo ERROR: Project file not found: %PROJECT_YML%
    exit /b 2
)
if not exist "%TYPEPHP_HOME%\tpc.exe" (
    echo ERROR: TypePHP compiler not found: %TYPEPHP_HOME%\tpc.exe
    exit /b 2
)

call "%VS_BUILD_TOOLS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%

if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"
if exist "%OUTPUT_EXE%" del /q "%OUTPUT_EXE%"

pushd "%TYPEPHP_HOME%"
"%TYPEPHP_HOME%\tpc.exe" "%PROJECT_YML%" --no-color
set "BUILD_EXIT=%ERRORLEVEL%"
popd
if not "%BUILD_EXIT%"=="0" exit /b %BUILD_EXIT%
if not exist "%OUTPUT_EXE%" (
    echo ERROR: TypePHP did not produce the expected GUI executable: %OUTPUT_EXE%
    exit /b 3
)

editbin /nologo /SUBSYSTEM:WINDOWS "%OUTPUT_EXE%"
if errorlevel 1 exit /b %errorlevel%

echo GUI executable: %OUTPUT_EXE%
exit /b 0
