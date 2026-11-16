@echo off
rem RealRTCW VR Hand Editor: pose the hands and fingers on each VR weapon, on your PC.
rem Opens in your browser. It never starts the game.
setlocal
cd /d "%~dp0game\tools\hand-editor"
set "PY=%USERPROFILE%\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe"
if exist "%PY%" goto run
set "PY="
for /f "delims=" %%p in ('where python 2^>nul') do if not defined PY set "PY=%%p"
if defined PY goto run
where py >nul 2>nul && (py -3 -u hand_editor_server.py & goto done)
echo Python 3 was not found. Install it from https://www.python.org/downloads/ and run this again.
pause
goto :eof
:run
"%PY%" -u hand_editor_server.py
:done
if errorlevel 1 pause
