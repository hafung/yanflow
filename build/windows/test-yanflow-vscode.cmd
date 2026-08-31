@echo off
setlocal
powershell -Sta -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-vscode.ps1"
exit /b %errorlevel%
