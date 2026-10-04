@echo off
rem Finds the RealRTCW co-op install (Steam) and puts it in COOP. Set COOP yourself before
rem calling this to use another location.
if defined COOP if exist "%COOP%\coopmain" goto :eof
set "COOP=%ProgramFiles(x86)%\Steam\steamapps\common\RealRTCW\coop"
if exist "%COOP%\coopmain" goto :eof
set "COOP="
for /f "usebackq delims=" %%P in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0find_coop.ps1"`) do set "COOP=%%P"
if not defined COOP (
  echo Could not find RealRTCW's co-op files. Install RealRTCW and its co-op mode from Steam,
  echo or edit game\find_coop.cmd and set COOP to the folder that contains "coopmain".
  pause
  exit /b 1
)
