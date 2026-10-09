@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy RemoteSigned -File "%~dp0Migrate.ps1" -Direction Down %*
set "migrationExitCode=%ERRORLEVEL%"
if "%~1"=="" pause
exit /b %migrationExitCode%
