@echo off
REM Double-click to download the GTA Advance room server's whole log and open it
REM in Notepad (a copy in your temp folder; the server keeps writing its own).
REM Logs in to host= from gta_server.txt (you will be asked for its password).
setlocal
call :readhost || goto :eof
set "OUT=%TEMP%\gta_rooms_server.log"
ssh -o StrictHostKeyChecking=accept-new %HOST% "gta-admin log" > "%OUT%"
if errorlevel 1 (
    echo Could not fetch the log. If gta-admin is missing, run gta_redeploy.bat once.
    pause
    goto :eof
)
start "" notepad "%OUT%"
goto :eof

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
