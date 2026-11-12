@echo off
rem Sets JAVA_EXE to a Java 25+ java.exe. Order: %MC_JAVA%, %JAVA_HOME%, common JDK install folders, PATH.
rem Minecraft 26.3 needs Java 25; an older Java on PATH (e.g. Java 8) fails with "class file version 69.0".
set "JAVA_EXE="
for %%J in ("%MC_JAVA%" "%JAVA_HOME%\bin\java.exe") do if not defined JAVA_EXE if exist "%%~J" call :check "%%~J"
for /d %%D in ("%ProgramFiles%\Eclipse Adoptium\jdk-2*" "%ProgramFiles%\Eclipse Adoptium\jre-2*" "%ProgramFiles%\Microsoft\jdk-2*" "%ProgramFiles%\Java\jdk-2*" "%ProgramFiles%\Zulu\zulu-2*" "%ProgramFiles%\Amazon Corretto\jdk2*" "%ProgramFiles%\BellSoft\LibericaJDK-2*") do if not defined JAVA_EXE if exist "%%~D\bin\java.exe" call :check "%%~D\bin\java.exe"
if not defined JAVA_EXE for /f "delims=" %%P in ('where java 2^>nul') do if not defined JAVA_EXE call :check "%%P"
if defined JAVA_EXE (echo Using Java: "%JAVA_EXE%") else (
  echo Java 25 or newer was not found. Install it from https://adoptium.net/ ^(Temurin 25^),
  echo or set "MC_JAVA=C:\path\to\jdk-25\bin\java.exe"
)
exit /b 0

:check
rem Accept the candidate if its major version is 25 or newer
set "CAND=%~1"
set "VER="
rem (extra outer quotes: cmd /c strips one pair, which keeps paths like "Program Files (x86)" intact)
for /f "tokens=3" %%V in ('""%CAND%" -version 2^>^&1 ^| findstr /i version"') do set "VER=%%~V"
if not defined VER exit /b 0
for /f "delims=." %%M in ("%VER%") do set "MAJOR=%%M"
if "%MAJOR%"=="1" exit /b 0
if %MAJOR% GEQ 25 set "JAVA_EXE=%CAND%"
exit /b 0
