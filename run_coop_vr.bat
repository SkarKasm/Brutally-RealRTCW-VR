@echo off
rem RealRTCW-VR co-op launcher: Cursed Sands co-op in VR.
rem   run_coop_vr.bat                 main menu (CREATE SERVER to host, JOIN GAME to join)
rem   run_coop_vr.bat +connect <ip>   join a friend's game directly
rem Game files come from the Steam RealRTCW co-op install (fs_basepath), the original RTCW
rem paks are found through the Steam library. Our DLLs, configs and saves live in .\run.
set "COOP=C:\Program Files (x86)\Steam\steamapps\common\RealRTCW\coop"
cd /d "%~dp0run"
start "" RTCWCoop.x64.exe +set fs_basepath "%COOP%" +set fs_homepath "%~dp0run" +set fs_game EE %*
