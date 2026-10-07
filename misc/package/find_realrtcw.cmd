@echo off
rem Finds the RealRTCW install (Steam) and puts it in RRTCW. Set RRTCW yourself before
rem calling this to use another location.
if defined RRTCW if exist "%RRTCW%\Main" goto :eof
set "RRTCW=%ProgramFiles(x86)%\Steam\steamapps\common\RealRTCW"
if exist "%RRTCW%\Main" goto :eof
set "RRTCW="
for /f "usebackq delims=" %%P in (`powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0find_realrtcw.ps1"`) do set "RRTCW=%%P"
if not defined RRTCW (
  echo Could not find RealRTCW. Install RealRTCW from Steam, or edit game\find_realrtcw.cmd
  echo and set RRTCW to the RealRTCW folder (the one that contains "Main").
  pause
  exit /b 1
)
