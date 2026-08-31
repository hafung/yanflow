@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0test-yanflow-textbox.ps1"
exit /b %errorlevel%
