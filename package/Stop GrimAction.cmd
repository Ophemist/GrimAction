@echo off
rem Stop GrimAction (optional): restores the normal camera while the game keeps running.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0bin\Stop-GrimAction.ps1" %*
echo.
pause
