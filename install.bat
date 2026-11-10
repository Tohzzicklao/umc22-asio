@echo off
rem Copia la DLL compilada a bin\ y la registra como driver ASIO (pide administrador).
rem Cierra Nolly X antes de ejecutarlo. Uso: install.bat  |  install.bat /u  (desinstalar)
cd /d %~dp0
if /i "%1"=="/u" (
  powershell -NoProfile -Command "Start-Process regsvr32 -ArgumentList '/s','/u','%~dp0bin\umc22asio.dll' -Verb RunAs -Wait"
  echo Desinstalado.
  exit /b
)
if not exist bin mkdir bin
copy /y umc22asio.dll bin\umc22asio.dll >nul || (echo No se pudo copiar: cierra Nolly X. & exit /b 1)
powershell -NoProfile -Command "Start-Process regsvr32 -ArgumentList '/s','%~dp0bin\umc22asio.dll' -Verb RunAs -Wait"
echo Instalado: "UMC22 Low Latency"
