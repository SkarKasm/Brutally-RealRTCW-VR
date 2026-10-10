@echo off
rem RealRTCW-VR: the Cursed Sands campaign (RealRTCW's EE folder) in VR
rem (falls back to the monitor without a headset).
call "%~dp0RealRTCW VR.bat" +set fs_game EE %*
