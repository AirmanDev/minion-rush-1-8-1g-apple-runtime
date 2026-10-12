@echo off
setlocal
set "kit=%~dp0"
if not exist "%kit%runtime\tools\windows_native_gui.ps1" (
  echo ERROR: The Windows graphical builder is missing.
  pause
  exit /b 1
)
start "" powershell.exe -NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "%kit%runtime\tools\windows_native_gui.ps1" -Kit "%kit%"
exit /b %ERRORLEVEL%
