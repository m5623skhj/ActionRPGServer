@echo off
setlocal

set "TOWN_SERVER_EXE=%~dp0ActionRPGServer\x64\Debug\TownServer.exe"
set "TOWN_SERVER_DIR=%~dp0ActionRPGServer\x64\Debug"
set "ROOM_SERVER_EXE=%~dp0artifacts\bin\x64\Debug\GameRoomServer.exe"
set "ROOM_SERVER_DIR=%~dp0artifacts\bin\x64\Debug"
set "CLIENT_EXE=%~dp0..\ActionRPGClient\ActionRPGClient\artifacts\bin\x64\Debug\ActionRPGClient.exe"
set "CLIENT_DIR=%~dp0..\ActionRPGClient\ActionRPGClient\artifacts\bin\x64\Debug"
set "TOWN_PORT=7777"
set "TOWN_CONTROL_PORT=7780"
set "IO_THREADS=4"

if not exist "%TOWN_SERVER_EXE%" (
    echo [ERROR] TownServer Debug executable was not found.
    echo         %TOWN_SERVER_EXE%
    echo Build TownServer Debug x64 first.
    pause
    exit /b 1
)

if not exist "%ROOM_SERVER_EXE%" (
    echo [ERROR] GameRoomServer Debug executable was not found.
    echo         %ROOM_SERVER_EXE%
    echo Build GameRoomServer Debug x64 first.
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
    "if (Get-NetTCPConnection -LocalPort %TOWN_PORT%,%TOWN_CONTROL_PORT% -State Listen -ErrorAction SilentlyContinue) { exit 1 }"
if errorlevel 1 (
    echo [ERROR] TCP port %TOWN_PORT% or %TOWN_CONTROL_PORT% is already in use.
    pause
    exit /b 1
)

rem Use a fresh shared secret for this launch; never write it to disk or echo it.
set "ACTIONRPG_ROOM_CONTROL_KEY="
for /f "delims=" %%K in ('powershell.exe -NoProfile -Command "$bytes = New-Object byte[] 32; $rng = [Security.Cryptography.RandomNumberGenerator]::Create(); try { $rng.GetBytes($bytes); [BitConverter]::ToString($bytes).Replace('-','').ToLowerInvariant() } finally { $rng.Dispose() }"') do set "ACTIONRPG_ROOM_CONTROL_KEY=%%K"
if not defined ACTIONRPG_ROOM_CONTROL_KEY (
    echo [ERROR] Unable to generate the room control authentication key.
    exit /b 1
)

echo [1/4] Starting TownServer on 127.0.0.1:%TOWN_PORT%...
start "TownServer" /D "%TOWN_SERVER_DIR%" cmd.exe /k ""%TOWN_SERVER_EXE%" %TOWN_PORT% %IO_THREADS% %TOWN_CONTROL_PORT%"

powershell.exe -NoProfile -Command ^
    "$deadline = (Get-Date).AddSeconds(10); while ((Get-Date) -lt $deadline) { try { $client = [Net.Sockets.TcpClient]::new(); $client.Connect('127.0.0.1', %TOWN_PORT%); $client.Dispose(); exit 0 } catch { if ($client) { $client.Dispose() }; Start-Sleep -Milliseconds 200 } }; exit 1"
if errorlevel 1 (
    echo [ERROR] TownServer did not start listening within 10 seconds.
    echo Check the TownServer window for the startup error.
    pause
    exit /b 1
)

echo [2/4] Starting GameRoomServer...
start "GameRoomServer" /D "%ROOM_SERVER_DIR%" cmd.exe /k ""%ROOM_SERVER_EXE%" 127.0.0.1 %TOWN_CONTROL_PORT% 1 1000 %IO_THREADS%"

echo [3/4] Starting client 1...
start "ActionRPGClient-1" /D "%CLIENT_DIR%" "%CLIENT_EXE%"

echo [4/4] Starting client 2...
start "ActionRPGClient-2" /D "%CLIENT_DIR%" "%CLIENT_EXE%"

echo.
echo TownServer, GameRoomServer and two clients were started.
echo Close each window when the local test is complete.
pause

endlocal
