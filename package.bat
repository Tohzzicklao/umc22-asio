@echo off
rem Packages a release: package.bat <version>
rem   dist\umc22-asio-v<version>-win64.zip    (portable: install.bat)
rem   dist\umc22-asio-v<version>-setup.exe    (installer, needs Inno Setup 6)
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

set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (echo Inno Setup 6 not found: installer skipped. & exit /b 0)
"%ISCC%" /Q /DAppVersion=%~1 installer\umc22asio.iss || exit /b 1
echo OK: dist\umc22-asio-v%~1-setup.exe
