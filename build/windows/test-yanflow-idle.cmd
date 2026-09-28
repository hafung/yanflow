@echo off
setlocal
set "YANFLOW_EXE=%~dp0..\artifacts\yanflow-windows-x64\yanflow.exe"
if not exist "%YANFLOW_EXE%" (
    echo ERROR: YanFlow package is missing. Run build-yanflow.cmd first.
    exit /b 2
)
start "" /wait "%YANFLOW_EXE%" --idle-smoke
if errorlevel 1 exit /b %errorlevel%
echo PASS yanflow-idle auto_stop=true
exit /b 0
