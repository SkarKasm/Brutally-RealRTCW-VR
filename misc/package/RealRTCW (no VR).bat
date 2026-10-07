@echo off
rem RealRTCW-VR on the monitor only (a headset is ignored), with the same extras.
call "%~dp0RealRTCW VR.bat" +set vr_enable 0 %*
