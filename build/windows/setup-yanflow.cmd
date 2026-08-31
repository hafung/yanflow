@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-yanflow.ps1" %*
exit /b %errorlevel%
