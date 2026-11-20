@echo off
REM Double-click for a live view of the GTA Advance room server's log: everything
REM since the server last started, then each new line as it happens (players
REM joining and leaving, rooms opening and closing, chat...). Scroll up for the
REM history; press Enter to stop. Logs in to host= from gta_server.txt (you will
REM be asked for its password). If the connection drops, press a key to
REM reconnect: the server keeps logging, so nothing is missed.
REM   gta_monitor.bat all     the whole log file, then live
REM   gta_monitor.bat 200     its last 200 lines, then live
setlocal
call :readhost || goto :eof
set "WHAT=%~1"
:again
ssh -t -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=20 -o ServerAliveCountMax=6 -o TCPKeepAlive=yes %HOST% "if command -v gta-admin >/dev/null; then gta-admin --watch %WHAT%; else echo 'gta-admin is not on this server yet: run gta_redeploy.bat once.'; fi"
echo.
echo The monitor closed. Press a key to open it again, or close this window.
pause >nul
goto again

:readhost
set "HOST="
if exist "%~dp0gta_server.txt" for /f "usebackq tokens=1,* delims==" %%a in (`findstr /b /i /c:"host" "%~dp0gta_server.txt"`) do set "HOST=%%b"
if defined HOST for /f "tokens=* delims= " %%a in ("%HOST%") do set "HOST=%%a"
if not defined HOST (
    echo No host= in gta_server.txt. Copy gta_server.example.txt to gta_server.txt and fill it in.
    pause
    exit /b 1
)
echo %HOST% | findstr "@" >nul || set "HOST=root@%HOST%"
exit /b 0
