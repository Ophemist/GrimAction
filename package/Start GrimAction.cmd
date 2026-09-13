@echo off
rem Start GrimAction: run this while Grim Dawn is at the main menu.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0bin\Start-GrimAction.ps1" %*
echo.
pause
