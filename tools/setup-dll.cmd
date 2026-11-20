@echo off
rem Wrapper for systems where PowerShell scripts are blocked (ExecutionPolicy Restricted).
rem
rem Usage:
rem   setup-dll.cmd "C:\path\to\game"            - check only
rem   setup-dll.cmd "C:\path\to\game" --apply    - swap with backup
setlocal
set "GAMEDIR=%~1"
set "EXTRA="
if /i "%~2"=="--apply" set "EXTRA=-Apply"
if "%GAMEDIR%"=="" (
    echo Usage: setup-dll.cmd "C:\path\to\game" [--apply]
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Setup-MatrixGameDll.ps1" -GameDir "%GAMEDIR%" %EXTRA%
