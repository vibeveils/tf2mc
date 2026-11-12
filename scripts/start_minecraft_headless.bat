@echo off
rem Starts the Minecraft 26.3 Fabric dedicated server with no GUI (tf2bridge in mods\).
if "%MC_DIR%"=="" set MC_DIR=%USERPROFILE%\mc-tf2
cd /d "%MC_DIR%"
if not exist eula.txt (echo Accept the Minecraft EULA first: create eula.txt containing eula=true & exit /b 1)
java -Xms2G -Xmx4G -jar fabric-server-launch.jar --nogui
