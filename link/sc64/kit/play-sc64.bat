@echo off
setlocal
title Halo 64 Online (SummerCart64)
rem Halo 64 Online on a real N64 with a SummerCart64: Halo 64 Link in
rem Ubuntu (WSL), then the bridge between the cart's USB port and the Link.
rem Close the bridge's window to stop everything. README.txt says more.
rem   play-sc64.bat                the internet's games
rem   play-sc64.bat local          a test game on this PC (play-sc64-local.bat)
rem   play-sc64.bat --port COM5    the cart on COM5 (the bridge's options:
rem                                --d2xx, --verbose too)
set "KIT=%~dp0."
set "MODE="
set "BRIDGEARGS="
:args
if "%~1"=="" goto args_done
if /i "%~1"=="local" (set "MODE=local") else (set "BRIDGEARGS=%BRIDGEARGS% %1")
shift
goto args
:args_done

if not exist "%KIT%\link\start-link.sh" goto not_unpacked
call :find_bridge || exit /b 1
call :wake || exit /b 1
call :stop
echo Starting Halo 64 Link in Ubuntu (its own window)...
start "Halo 64 Link" wsl.exe %WSLD% --cd "%KIT%" -e bash link/start-link.sh %MODE%
call :wait_for_link || exit /b 1
echo Starting the SummerCart64 bridge (its own window): close that window,
echo or press Ctrl+C in it, to stop everything.
start "SummerCart64 bridge" /wait cmd /c "%BRIDGE% --link 127.0.0.1:6466 %BRIDGEARGS% || pause"
echo The bridge stopped: stopping Halo 64 Link...
call :stop
exit /b 0

:not_unpacked
echo This folder is missing files: unpack the whole zip into a folder first
echo (right-click the zip, Extract All), then run play-sc64.bat from there.
pause
exit /b 1

:find_bridge
rem (the bridge as a program, else its Python source with Python 3)
if exist "%KIT%\sc64\sc64_bridge.exe" (
	set BRIDGE="%KIT%\sc64\sc64_bridge.exe"
	exit /b 0
)
if not exist "%KIT%\sc64\sc64_bridge.py" goto not_unpacked
py -3 -c "import sys" >nul 2>&1
if not errorlevel 1 (
	set BRIDGE=py -3 "%KIT%\sc64\sc64_bridge.py"
	exit /b 0
)
python -c "import sys; sys.exit(sys.version_info[0] < 3)" >nul 2>&1
if not errorlevel 1 (
	set BRIDGE=python "%KIT%\sc64\sc64_bridge.py"
	exit /b 0
)
echo The bridge needs sc64\sc64_bridge.exe, or Python 3 (python.org, with
echo "Add python.exe to PATH" ticked) for sc64\sc64_bridge.py.
pause
exit /b 1

:wake
rem (Ubuntu if there is one, else WSL's default; starting it can time out
rem when the PC is busy: a few tries)
echo Starting Ubuntu (WSL)...
set tries=0
:wake_again
set "WSLD=-d Ubuntu"
wsl.exe %WSLD% -e true >nul 2>&1
if not errorlevel 1 exit /b 0
set "WSLD="
wsl.exe -e true >nul 2>&1
if not errorlevel 1 exit /b 0
set /a tries+=1
if %tries% geq 4 goto wake_failed
echo   Ubuntu did not start yet, trying again...
timeout /t 5 /nobreak >nul
goto wake_again
:wake_failed
echo.
echo Ubuntu (WSL) would not start. If it isn't installed, open a command prompt
echo and run:  wsl --install   (then restart the PC and open Ubuntu once).
echo If it is, run:  wsl --shutdown   and try again.
pause
exit /b 1

:wait_for_link
set waited=0
:wait_again
timeout /t 2 /nobreak >nul
wsl.exe %WSLD% --cd "%KIT%" -e bash link/waiting.sh >nul 2>&1
if not errorlevel 1 exit /b 0
set /a waited+=2
if %waited% lss 90 goto wait_again
echo.
echo Halo 64 Link did not start: see the "Halo 64 Link" window, or data\link.log.
call :stop
pause
exit /b 1

:stop
wsl.exe %WSLD% --cd "%KIT%" -e bash link/stop-link.sh >nul 2>&1
exit /b 0
