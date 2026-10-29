@echo off
rem Balatro360: copies the game files out of your own copy of Balatro into the
rem Balatro360 folder next to this file. Nothing to install: it uses the
rem PowerShell that comes with Windows 10 and 11.
if not exist "%~dp0converter\convert.ps1" (
    echo The converter folder is missing. Extract the whole zip file first,
    echo then run "Convert Balatro.bat" from the extracted folder.
    pause
    exit /b 1
)
rem Balatro.exe dropped on this file skips the file dialog.
if "%~1"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0converter\convert.ps1"
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -STA -File "%~dp0converter\convert.ps1" -Source "%~1"
)
pause
