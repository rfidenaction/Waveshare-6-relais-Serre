@echo off
setlocal
cd /d "%~dp0"

REM Lance le script PowerShell (dialogues HTML/icone + compilation)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\android-serre\Compiler.ps1"
set ERR=%ERRORLEVEL%

echo.
if %ERR% neq 0 (
  echo Echec de la compilation (code %ERR%).
) else (
  echo Termine.
)
pause
exit /b %ERR%
