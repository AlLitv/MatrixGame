@echo off
rem Launches Space Rangers HD with the determinism harness enabled in MatrixGame.dll.
rem Logs are written next to this script into the .\logs subfolder.
rem Usage: start-battle.cmd run1
setlocal
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Space Rangers HD A War Apart"
if "%~1"=="" (
    echo Usage: start-battle.cmd run1
    exit /b 1
)
if not exist "%~dp0logs" mkdir "%~dp0logs"
set "MATRIXGAME_SIM=1"
set "MATRIXGAME_SEED=12345"
set "MATRIXGAME_FIXEDSTEP=10"
set "MATRIXGAME_MAXTICKS=30000"
set "MATRIXGAME_MAP=armagedd"
set "MATRIXGAME_HASHLOG=%~dp0logs\%~1.txt"
set "MATRIXGAME_OBJLOG=%~dp0logs\%~1-obj.txt"
set "MATRIXGAME_TRACE=%~dp0logs\%~1-trace.txt"
start "" "%GAME%\Rangers.exe"
