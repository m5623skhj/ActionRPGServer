@echo off
setlocal

set "SERVER_EXE=%~dp0ActionRPGServer\x64\Debug\TownServer.exe"
set "SERVER_DIR=%~dp0ActionRPGServer\TownServer\x64\Debug"
set "CLIENT_EXE=%~dp0..\ActionRPGClient\ActionRPGClient\artifacts\bin\x64\Debug\ActionRPGClient.exe"
set "CLIENT_DIR=%~dp0..\ActionRPGClient\ActionRPGClient\artifacts\bin\x64\Debug"
set "TOWN_PORT=7777"
set "IO_THREADS=4"

if not exist "%SERVER_EXE%" (
    echo [ERROR] TownServer Debug executable was not found.
    echo         %SERVER_EXE%
    echo Build TownServer Debug x64 first.
    pause
    exit /b 1
)

if not exist "%CLIENT_EXE%" (
    echo [ERROR] ActionRPGClient Debug executable was not found.
    echo         %CLIENT_EXE%
    echo Build ActionRPGClient Debug x64 first.
    pause
    exit /b 1
)

powershell.exe -NoProfile -Command ^
    "if (Get-NetTCPConnection -LocalPort %TOWN_PORT% -State Listen -ErrorAction SilentlyContinue) { exit 1 }"
if errorlevel 1 (
    echo [ERROR] TCP port %TOWN_PORT% is already in use.
    pause
    exit /b 1
)

echo [1/3] Starting TownServer on 127.0.0.1:%TOWN_PORT%...
start "TownServer" /D "%SERVER_DIR%" cmd.exe /k ""%SERVER_EXE%" %TOWN_PORT% %IO_THREADS%"

powershell.exe -NoProfile -Command ^
    "$deadline = (Get-Date).AddSeconds(10); while ((Get-Date) -lt $deadline) { try { $client = [Net.Sockets.TcpClient]::new(); $client.Connect('127.0.0.1', %TOWN_PORT%); $client.Dispose(); exit 0 } catch { if ($client) { $client.Dispose() }; Start-Sleep -Milliseconds 200 } }; exit 1"
if errorlevel 1 (
    echo [ERROR] TownServer did not start listening within 10 seconds.
    echo Check the TownServer window for the startup error.
    pause
    exit /b 1
)

echo [2/3] Starting client 1...
start "ActionRPGClient-1" /D "%CLIENT_DIR%" "%CLIENT_EXE%"

echo [3/3] Starting client 2...
start "ActionRPGClient-2" /D "%CLIENT_DIR%" "%CLIENT_EXE%"

echo.
echo TownServer and two clients were started.
echo Close each window when the local test is complete.
pause

endlocal
