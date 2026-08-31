@echo off
setlocal

if "%~2"=="" (
    echo Usage: stage-typephp-gui-runtime.cmd OUTPUT_EXE PACKAGE_DIR [RUNTIME_DIR]
    exit /b 2
)

for %%I in ("%~1") do set "OUTPUT_EXE=%%~fI"
for %%I in ("%~2") do set "PACKAGE_DIR=%%~fI"
if not "%~3"=="" set "RUNTIME_DIR=%~3"
if not defined RUNTIME_DIR if defined TYPEPHP_RUNTIME_DIR set "RUNTIME_DIR=%TYPEPHP_RUNTIME_DIR%"
if not defined RUNTIME_DIR if defined TYPEPHP_HOME set "RUNTIME_DIR=%TYPEPHP_HOME%"
if not defined RUNTIME_DIR (
    echo ERROR: Set TYPEPHP_RUNTIME_DIR or TYPEPHP_HOME.
    exit /b 2
)

if not exist "%OUTPUT_EXE%" (
    echo ERROR: GUI executable not found: %OUTPUT_EXE%
    exit /b 2
)
if not exist "%PACKAGE_DIR%" mkdir "%PACKAGE_DIR%"

copy /y "%OUTPUT_EXE%" "%PACKAGE_DIR%\%~nx1" >nul
if errorlevel 1 exit /b %errorlevel%
for %%D in (phpx.dll php8ts.dll gmp-10.dll mpfr-6.dll libmpdec-4.0.1.dll libmpdec++-4.0.1.dll) do (
    if not exist "%RUNTIME_DIR%\%%D" (
        echo ERROR: Missing runtime DLL: %RUNTIME_DIR%\%%D
        exit /b 3
    )
    copy /y "%RUNTIME_DIR%\%%D" "%PACKAGE_DIR%\%%D" >nul
    if errorlevel 1 exit /b 3
)
if exist "%RUNTIME_DIR%\LICENSE" copy /y "%RUNTIME_DIR%\LICENSE" "%PACKAGE_DIR%\LICENSE-TypePHP.txt" >nul
if exist "%RUNTIME_DIR%\license.txt" copy /y "%RUNTIME_DIR%\license.txt" "%PACKAGE_DIR%\LICENSE-PHP.txt" >nul

echo Staged TypePHP GUI runtime: %PACKAGE_DIR%
exit /b 0
