@echo off
rem Builds the driver (umc22asio.dll) and the diagnostic tools (tools\bin\*.exe) with MSVC x64.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo Visual Studio with C++ tools not found. & exit /b 1)
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d %~dp0

cl /nologo /std:c++17 /EHsc /O2 /W3 /MT /LD umc22asio.cpp /link /DEF:umc22asio.def /OUT:umc22asio.dll || exit /b 1

if not exist tools\bin mkdir tools\bin
for %%t in (testhost probe phasetest) do (
  cl /nologo /std:c++17 /EHsc /O2 /MT tools\%%t.cpp /Fo:tools\bin\ /Fe:tools\bin\%%t.exe >nul || (echo Failed to build %%t & exit /b 1)
)
echo OK: umc22asio.dll and tools\bin\*.exe
