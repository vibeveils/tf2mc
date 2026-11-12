@echo off
rem Compiles maps\src\mc_world.vmf (an empty sealed sky box) with the SDK map tools.
rem   compile_map.bat "<Steam>\steamapps\common\Source SDK Base 2013 Multiplayer" "C:\src\source-sdk-2013\game\mod_tf"
rem Arg 1: Source SDK Base 2013 Multiplayer folder (its bin\x64 or bin holds vbsp/vvis/vrad)
rem Arg 2: the mod folder (contains gameinfo.txt and maps\src\mc_world.vmf)
rem TF2DIR defaults to the Team Fortress 2 folder next to Source SDK Base in the same Steam library.
setlocal
set "SDKBASE=%~1"
set "MOD=%~2"
if "%MOD%"=="" (echo usage: compile_map.bat ^<Source SDK Base 2013 Multiplayer folder^> ^<mod folder^> & exit /b 1)
if defined TF2DIR set "TF2DIR=%TF2DIR:"=%"
if not defined TF2DIR set "TF2DIR=%SDKBASE%\..\Team Fortress 2"
set "TOOLS="
if exist "%SDKBASE%\bin\x64\vbsp.exe" set "TOOLS=%SDKBASE%\bin\x64"
if not defined TOOLS if exist "%SDKBASE%\bin\vbsp.exe" set "TOOLS=%SDKBASE%\bin"
if not defined TOOLS (echo vbsp.exe not found under "%SDKBASE%\bin" & exit /b 1)
if not exist "%TF2DIR%\tf\tf2_misc_dir.vpk" (echo Team Fortress 2 not found at "%TF2DIR%" - set TF2DIR & exit /b 1)

rem The tools can't use the mod's "|appid_440|" mounts, so mount the same content by absolute path.
set "CG=%MOD%\maps\_compile"
if not exist "%CG%" mkdir "%CG%"
set "P_TF=%TF2DIR:\=/%/tf"
set "P_HL2=%SDKBASE:\=/%/hl2"
set "P_MOD=%MOD:\=/%"
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
set "VMF=%MOD%\maps\src\mc_world"
"%TOOLS%\vbsp.exe" -game "%CG%" "%VMF%.vmf" || exit /b 1
"%TOOLS%\vvis.exe" -fast -game "%CG%" "%VMF%" || exit /b 1
"%TOOLS%\vrad.exe" -fast -game "%CG%" "%VMF%" || exit /b 1
copy /Y "%VMF%.bsp" "%MOD%\maps\mc_world.bsp" >nul && echo maps\mc_world.bsp ready
endlocal
