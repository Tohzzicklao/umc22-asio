@echo off
rem Copies umc22asio.dll to bin\ and registers it as an ASIO driver (asks for admin).
rem Close your ASIO host (Nolly X, DAW...) first.  Usage: install.bat  |  install.bat /u  (uninstall)
cd /d %~dp0
if /i "%1"=="/u" (
  powershell -NoProfile -Command "Start-Process regsvr32 -ArgumentList '/s','/u','%~dp0bin\umc22asio.dll' -Verb RunAs -Wait"
  echo Uninstalled.
  exit /b
)
if not exist umc22asio.dll (echo umc22asio.dll not found next to install.bat. & exit /b 1)
if not exist bin mkdir bin
copy /y umc22asio.dll bin\umc22asio.dll >nul || (echo Could not copy the DLL: close your ASIO host and try again. & exit /b 1)
powershell -NoProfile -Command "Start-Process regsvr32 -ArgumentList '/s','%~dp0bin\umc22asio.dll' -Verb RunAs -Wait"
echo Installed: "UMC22 Low Latency"
