@echo off
rem Replaces Grim Dawn's hand cursor with a white dot. Close Grim Dawn first. Undo with "Restore Hand Cursor".
rem Dot size, outline and shape are settings at the top of Set-HiddenHandCursor.ps1.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Set-HiddenHandCursor.ps1" -Style Dot %*
echo.
pause
