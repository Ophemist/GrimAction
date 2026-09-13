@echo off
rem Puts Grim Dawn's original hand cursor back. Close Grim Dawn first.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Set-HiddenHandCursor.ps1" -Restore %*
echo.
pause
