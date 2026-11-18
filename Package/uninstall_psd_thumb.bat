@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_psd_thumb.ps1"
set "result=%ERRORLEVEL%"
pause
exit /b %result%
