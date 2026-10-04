@echo off
rem RealRTCW-VR launcher.
rem Assets come from the Steam RealRTCW install (fs_basepath); vanilla RTCW paks are found
rem automatically through the Steam library scan. Our own DLLs/configs/saves live in .\run.
set "RRTCW=C:\Program Files (x86)\Steam\steamapps\common\RealRTCW"
set "PATH=%RRTCW%;%PATH%"
cd /d "%~dp0run"
start "" RealRTCW.x64.exe +set fs_basepath "%RRTCW%" +set fs_homepath "%~dp0run" %*
