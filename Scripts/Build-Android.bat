@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Android.ps1" %*
exit /b %errorlevel%
