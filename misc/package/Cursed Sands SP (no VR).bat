@echo off
rem RealRTCW-VR: the Cursed Sands campaign on the monitor only (a headset is ignored).
call "%~dp0RealRTCW VR.bat" +set fs_game EE +set vr_enable 0 %*
