@echo off
setlocal
powershell -Sta -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-ui.ps1"
exit /b %errorlevel%
