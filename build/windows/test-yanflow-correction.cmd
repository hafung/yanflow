@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-correction.ps1"
exit /b %errorlevel%
