@echo off
rem Compila el driver (umc22asio.dll) y las herramientas de diagnostico (tools\*.exe) con MSVC x64.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No se encontro Visual Studio con C++. & exit /b 1)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d %~dp0

cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /LD umc22asio.cpp /link /DEF:umc22asio.def /OUT:umc22asio.dll || exit /b 1

if not exist tools\bin mkdir tools\bin
for %%t in (testhost probe phasetest) do (
  cl /nologo /std:c++17 /EHsc /O2 /MT tools\%%t.cpp /Fo:tools\bin\ /Fe:tools\bin\%%t.exe >nul || (echo Error compilando %%t & exit /b 1)
)
echo OK: umc22asio.dll y tools\bin\*.exe
