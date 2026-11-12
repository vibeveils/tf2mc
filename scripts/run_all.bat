@echo off
rem TF2MC one-click launcher for Windows:
rem   1. starts the headless Minecraft 26.3 server (tf2bridge in mods\) in its own window
rem   2. waits until the bridge port answers
rem   3. starts the TF2 mod on mc_world
rem Settings (edit or set before running):
rem   MC_DIR   folder of the Fabric server          (default %USERPROFILE%\mc-tf2)
rem   GAME_DIR folder containing mod_tf_win64.exe   (default: the SDK's "game" folder next to this repo)
setlocal
if "%MC_DIR%"=="" set MC_DIR=%USERPROFILE%\mc-tf2
if "%GAME_DIR%"=="" set GAME_DIR=%~dp0..\..\source-sdk-2013\game
if "%MC_PORT%"=="" set MC_PORT=25599

if not exist "%MC_DIR%\fabric-server-launch.jar" (
  echo Minecraft server not found in "%MC_DIR%". See README: "Run on Windows".
  exit /b 1
)
if not exist "%GAME_DIR%\mod_tf_win64.exe" (
  echo mod_tf_win64.exe not found in "%GAME_DIR%". Set GAME_DIR to your SDK "game" folder.
  exit /b 1
)

call "%~dp0find_java.bat"
if not defined JAVA_EXE exit /b 1
echo Starting Minecraft server (headless) ...
start "TF2MC Minecraft server" /D "%MC_DIR%" "%JAVA_EXE%" -Xms2G -Xmx4G -jar fabric-server-launch.jar --nogui

echo Waiting for the bridge on port %MC_PORT% ...
powershell -NoProfile -Command "$t=0; while(-not (Test-NetConnection 127.0.0.1 -Port %MC_PORT% -InformationLevel Quiet -WarningAction SilentlyContinue)) { Start-Sleep 2; $t+=2; if($t -gt 300){ exit 1 } }"
if errorlevel 1 (
  echo The bridge did not come up within 5 minutes - check the Minecraft server window.
  exit /b 1
)

echo Bridge is up - launching TF2MC
start "" /D "%GAME_DIR%" "%GAME_DIR%\mod_tf_win64.exe" -novid -insecure +exec tf2mc_server.cfg +map mc_world
endlocal
