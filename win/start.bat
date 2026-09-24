@echo off
rem DAGCore Miner - start as administrator.
rem
rem The power limit can only be changed by a miner running as administrator
rem (nvidia-smi -pl needs it on Windows). This file asks Windows for that -
rem the UAC prompt - and starts dagcore-miner.exe from this folder in a window
rem of its own, with any arguments given here:  start.bat --threads 1
rem
rem The miner gets the window to itself, as with a double-click: Ctrl+C stops
rem it without a "Terminate batch job" question, an error stays on screen,
rem and Save & restart restarts it there.
rem
rem Without administrator rights the miner works too, only the power limit is
rem then unavailable: double-clicking dagcore-miner.exe is enough for that.

rem fltmc answers only an administrator.
fltmc >nul 2>&1
if not errorlevel 1 (
    start "DAGCore Miner" /d "%~dp0" "%~dp0dagcore-miner.exe" %*
    goto :eof
)

if "%~1"=="" (
    powershell -NoProfile -Command "Start-Process -FilePath '%~dp0dagcore-miner.exe' -Verb RunAs"
) else (
    powershell -NoProfile -Command "Start-Process -FilePath '%~dp0dagcore-miner.exe' -ArgumentList '%*' -Verb RunAs"
)
if errorlevel 1 (
    echo.
    echo Administrator rights were not granted, so the miner was not started.
    echo To mine without them, double-click dagcore-miner.exe instead - only
    echo the power limit needs them.
    echo.
    pause
)
