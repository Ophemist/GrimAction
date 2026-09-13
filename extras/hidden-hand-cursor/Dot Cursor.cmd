@echo off
rem Replaces Grim Dawn's hand cursor with a small white dot. Close Grim Dawn first. Undo with "Restore Hand Cursor".
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Set-HiddenHandCursor.ps1" -Style Dot %*
echo.
pause
