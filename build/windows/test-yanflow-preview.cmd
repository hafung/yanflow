@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-preview.ps1"
exit /b %errorlevel%
