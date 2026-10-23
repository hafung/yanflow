@echo off
setlocal
py.exe -3 "%~dp0..\..\tests\prepared-asr-smoke.py" "%~dp0..\.."
exit /b %errorlevel%
