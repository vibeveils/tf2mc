@echo off
rem Copies TF2MC into a source-sdk-2013 clone and applies the TF2 patch.  install_into_sdk.bat C:\src\source-sdk-2013
if "%~1"=="" (echo usage: install_into_sdk.bat ^<path to source-sdk-2013^> & exit /b 1)
set SDK=%~1
set HERE=%~dp0..
xcopy /E /I /Y "%HERE%\source\src" "%SDK%\src" >nul
xcopy /E /I /Y "%HERE%\game\mod_tf" "%SDK%\game\mod_tf" >nul
pushd "%SDK%"
git apply --ignore-whitespace -p1 "%HERE%\source\patches\tf2mc.patch" && echo patch applied || python "%HERE%\scripts\apply_tf2_hooks.py" "%SDK%"
if errorlevel 1 (popd & exit /b 1)
popd
echo Now run src\createallprojects.bat and build everything.sln
