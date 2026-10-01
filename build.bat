@echo off
rem Builds build\dinput8.dll (32-bit, static CRT) and the test host with MSVC.
setlocal
set VCVARS=
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (echo MSVC with the x86 tools not found & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /Fobuild\ ^
   src\dllmain.cpp src\hooks.cpp src\loose.cpp src\arc_patch.cpp ^
   /LD /Fe:build\dinput8.dll /link /DEF:src\dinput8.def /MACHINE:X86 kernel32.lib user32.lib || exit /b 1
cl /nologo /O2 /MT /EHsc /W4 /Fobuild\ test\test_host.cpp /Fe:build\test_host.exe /link /MACHINE:X86 || exit /b 1
echo built build\dinput8.dll
