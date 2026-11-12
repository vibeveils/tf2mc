@echo off
rem Installs a CI-built TF2MC mod into Steam's sourcemods folder and compiles the map.
rem   install_sourcemod.bat "C:\Downloads\tf2-mod-windows"
rem The argument is the downloaded folder that contains gameinfo.txt (or a "game" folder with mod_tf\ inside).
rem Needs Steam with Team Fortress 2 and Source SDK Base 2013 Multiplayer installed.
setlocal EnableDelayedExpansion
set "SRC=%~1"
if "%SRC%"=="" (echo usage: install_sourcemod.bat ^<folder with gameinfo.txt^> & exit /b 1)
if not exist "%SRC%\gameinfo.txt" if exist "%SRC%\mod_tf\gameinfo.txt" set "SRC=%SRC%\mod_tf"
if not exist "%SRC%\gameinfo.txt" (echo No gameinfo.txt in "%SRC%" & exit /b 1)
if not exist "%SRC%\bin\x64\client.dll" (
  echo "%SRC%\bin\x64\client.dll" is missing - use the *Windows* build artifact ^(tf2-mod-windows^).
  exit /b 1
)

rem --- Steam location (auto-detected from the registry; STEAM / SDKBASE may be set with or without quotes)
if defined STEAM set "STEAM=%STEAM:"=%"
if defined SDKBASE set "SDKBASE=%SDKBASE:"=%"
if not defined STEAM for /f "tokens=2,*" %%a in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul ^| find "SteamPath"') do set "STEAM=%%b"
set "STEAM=%STEAM:/=\%"
if "%STEAM:~-1%"=="\" set "STEAM=%STEAM:~0,-1%"
if not exist "%STEAM%\steam.exe" (echo Steam not found at "%STEAM%" - set "STEAM=C:\path\to\Steam" & exit /b 1)
echo Steam: "%STEAM%"
set "DEST=%STEAM%\steamapps\sourcemods\tf2mc"

rem --- Team Fortress 2 (its content is needed to compile the map); set TF2DIR if it's in another Steam library
if defined TF2DIR set "TF2DIR=%TF2DIR:"=%"
if not defined TF2DIR set "TF2DIR=%STEAM%\steamapps\common\Team Fortress 2"

rem --- Source SDK Base 2013 Multiplayer (map tools); set SDKBASE if it's in another Steam library
if not defined SDKBASE set "SDKBASE=%STEAM%\steamapps\common\Source SDK Base 2013 Multiplayer"
set "TOOLS="
if exist "%SDKBASE%\bin\x64\vbsp.exe" set "TOOLS=%SDKBASE%\bin\x64"
if not defined TOOLS if exist "%SDKBASE%\bin\vbsp.exe" set "TOOLS=%SDKBASE%\bin"

echo Copying mod to "%DEST%" ...
xcopy /E /I /Y /Q "%SRC%" "%DEST%" >nul || exit /b 1

rem Give it a recognisable name in the Steam library
powershell -NoProfile -Command "(Get-Content -Raw '%DEST%\gameinfo.txt') -replace '\"Frog Fortress 2\"','\"TF2MC (TF2 x Minecraft)\"' | Set-Content -NoNewline '%DEST%\gameinfo.txt'"

if exist "%DEST%\maps\mc_world.bsp" (echo Map already compiled.) else (
  if not defined TOOLS (
    echo Could not find vbsp.exe under "%SDKBASE%\bin". Install Source SDK Base 2013 Multiplayer,
    echo or set SDKBASE to its folder, then run this again.
    exit /b 1
  )
  if not exist "%TF2DIR%\tf\tf2_misc_dir.vpk" (
    echo Team Fortress 2 not found at "%TF2DIR%" - set "TF2DIR=D:\SteamLibrary\steamapps\common\Team Fortress 2"
    exit /b 1
  )
  call :compile_map || exit /b 1
  copy /Y "%DEST%\maps\src\mc_world.bsp" "%DEST%\maps\mc_world.bsp" >nul || exit /b 1
  echo Map compiled: maps\mc_world.bsp
)

echo.
echo Done. Restart Steam: "TF2MC (TF2 x Minecraft)" appears in your library.
echo Set its launch options to:   -insecure +exec tf2mc_server.cfg +map mc_world
echo Start the Minecraft server first (start_minecraft_headless.bat), then play from Steam.
endlocal
exit /b 0

rem ---------------------------------------------------------------------------
rem The map tools can't use the mod's gameinfo.txt: its "|appid_440|" mounts only work inside
rem the engine ("Appid based mounting is not supported on non-engine DLL projects"). Compile with a
rem temporary gameinfo that mounts the same TF2 / SDK Base content by absolute path.
:compile_map
set "CG=%DEST%\maps\_compile"
if not exist "%CG%" mkdir "%CG%"
set "P_TF=%TF2DIR:\=/%/tf"
set "P_HL2=%SDKBASE:\=/%/hl2"
set "P_MOD=%DEST:\=/%"
> "%CG%\gameinfo.txt" (
  echo "GameInfo"
  echo {
  echo 	game	"TF2MC map compile"
  echo 	FileSystem
  echo 	{
  echo 		SteamAppId	243750
  echo 		SearchPaths
  echo 		{
  echo 			game	"%P_MOD%"
  echo 			game	"%P_TF%/tf2_misc.vpk"
  echo 			game	"%P_TF%/tf2_textures.vpk"
  echo 			game	"%P_HL2%/hl2_misc.vpk"
  echo 			game	"%P_HL2%/hl2_textures.vpk"
  echo 			platform	"%SDKBASE:\=/%/platform/platform_misc.vpk"
  echo 		}
  echo 	}
  echo }
)
echo Compiling mc_world (vbsp / vvis / vrad) ...
"%TOOLS%\vbsp.exe" -game "%CG%" "%DEST%\maps\src\mc_world.vmf" || exit /b 1
"%TOOLS%\vvis.exe" -fast -game "%CG%" "%DEST%\maps\src\mc_world" || exit /b 1
"%TOOLS%\vrad.exe" -fast -game "%CG%" "%DEST%\maps\src\mc_world" || exit /b 1
exit /b 0
