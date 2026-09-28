@echo off
setlocal
where python >nul 2>nul
if not errorlevel 1 goto run_python

where py >nul 2>nul
if not errorlevel 1 goto run_py

echo Python 3 was not found. Install Tool\PacketGenerator\requirements.txt after installing Python. 1>&2
exit /b 1

:run_python
python "%~dp0PacketGenerator\PacketGenerator.py" %*
exit /b %ERRORLEVEL%

:run_py
py -3 "%~dp0PacketGenerator\PacketGenerator.py" %*
exit /b %ERRORLEVEL%
