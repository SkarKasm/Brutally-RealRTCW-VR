@echo off
rem RealRTCW-VR on the monitor only (headset ignored for this launch).
call "%~dp0run_vr.bat" +set vr_enable 0 %*
