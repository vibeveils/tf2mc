@echo off
rem Starts the Minecraft 26.3 Fabric dedicated server with no GUI (tf2bridge in mods\).
rem Needs Java 25+: found automatically, or set "MC_JAVA=C:\path\to\java.exe"
setlocal
if "%MC_DIR%"=="" set "MC_DIR=%USERPROFILE%\mc-tf2"
call "%~dp0find_java.bat"
if not defined JAVA_EXE exit /b 1
cd /d "%MC_DIR%" || exit /b 1
if not exist eula.txt (echo Accept the Minecraft EULA first: create eula.txt containing eula=true & exit /b 1)
"%JAVA_EXE%" -Xms2G -Xmx4G -jar fabric-server-launch.jar --nogui
endlocal
