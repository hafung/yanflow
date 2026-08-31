@echo off
setlocal
powershell -Sta -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-notepad.ps1"
exit /b %errorlevel%
