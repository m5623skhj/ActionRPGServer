@echo off
setlocal
where python >nul 2>nul
if not errorlevel 1 goto run_python

where py >nul 2>nul
if not errorlevel 1 goto run_py

echo Python 3 was not found. Install Tool\PacketGenerator\requirements.txt after installing Python. 1>&2
exit /b 1

:run_python
python "%~dp0PacketGenerator\PacketGenerator.py" --schema "%~dp0PacketDefine.yml" %*
if errorlevel 1 exit /b %ERRORLEVEL%
python "%~dp0PacketGenerator\PacketGenerator.py" --schema "%~dp0TownPacketDefine.yml" %*
exit /b %ERRORLEVEL%

:run_py
py -3 "%~dp0PacketGenerator\PacketGenerator.py" --schema "%~dp0PacketDefine.yml" %*
if errorlevel 1 exit /b %ERRORLEVEL%
py -3 "%~dp0PacketGenerator\PacketGenerator.py" --schema "%~dp0TownPacketDefine.yml" %*
exit /b %ERRORLEVEL%
