@echo off
setlocal
powershell.exe -NoProfile -File "%~dp0Migrate.ps1" -Direction Up %*
exit /b %ERRORLEVEL%
