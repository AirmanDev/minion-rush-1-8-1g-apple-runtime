@echo off
setlocal
pushd "%~dp0" || (
  echo ERROR: Cannot access the portable Windows kit folder.
  pause
  exit /b 1
)
echo Minion Rush 1.8.1g - private native ARM64 IPA builder
echo The game ZIP stays local and no files are uploaded.
powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -File "%CD%\run-native-ipa.ps1"
set "result=%ERRORLEVEL%"
popd
echo.
if "%result%"=="0" (
  echo Native unsigned IPA successfully prepared.
) else (
  echo Windows native IPA preparation failed. See error output above.
)
pause
exit /b %result%
