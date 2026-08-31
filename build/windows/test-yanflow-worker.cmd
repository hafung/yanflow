@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-worker.ps1" %*
exit /b %errorlevel%
