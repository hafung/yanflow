@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-pipeline.ps1" -Hold
exit /b %errorlevel%
