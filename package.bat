@echo off
rem Packages a release zip: package.bat <version>   ->  dist\umc22-asio-v<version>-win64.zip
setlocal
if "%~1"=="" (echo Usage: package.bat ^<version^> & exit /b 1)
cd /d %~dp0
if not exist umc22asio.dll (echo Run build.bat first. & exit /b 1)
set "NAME=umc22-asio-v%~1-win64"
set "STAGE=dist\%NAME%"
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\tools"
copy /y umc22asio.dll "%STAGE%\" >nul
copy /y install.bat "%STAGE%\" >nul
copy /y README.md "%STAGE%\" >nul
copy /y CHANGELOG.md "%STAGE%\" >nul
copy /y LICENSE "%STAGE%\" >nul
copy /y tools\bin\*.exe "%STAGE%\tools\" >nul
if exist "dist\%NAME%.zip" del "dist\%NAME%.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%STAGE%\*' -DestinationPath 'dist\%NAME%.zip'" || exit /b 1
echo OK: dist\%NAME%.zip
