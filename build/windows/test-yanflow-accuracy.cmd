@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-accuracy.ps1"
exit /b %errorlevel%
