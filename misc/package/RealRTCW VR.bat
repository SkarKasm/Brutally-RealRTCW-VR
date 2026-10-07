@echo off
rem RealRTCW-VR: the single player campaign in VR (falls back to the monitor without a headset).
rem Assets come from your Steam RealRTCW install; nothing there is changed. Settings and
rem saves are kept in the "game" folder next to this file.
call "%~dp0game\find_realrtcw.cmd" || exit /b 1
set "PATH=%RRTCW%;%PATH%"
cd /d "%~dp0game"
start "" RealRTCW.x64.exe +set fs_basepath "%RRTCW%" +set fs_homepath "%~dp0game" %*
