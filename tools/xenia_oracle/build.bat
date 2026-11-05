@echo off
rem Builds ppc_oracle.exe against the xenia-canary built in ..\..\..\xenia-canary
rem (Release, MSVC: its xenia-cpu, xenia-base and fmt libraries), into
rem ..\..\..\external\oracle. Not part of XenoForge's build: an oracle for
rem checking it (docs/recompiler.md, "Checks").
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set X=%~dp0..\..\..\xenia-canary
set OUT=%~dp0..\..\..\external\oracle
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"
cl /nologo /O2 /MD /EHsc /std:c++20 /utf-8 /DNDEBUG /DWIN32 /D_WIN64=1 /D_AMD64=1 /DUNICODE /D_UNICODE /D_LIB ^
  /DXE_OPTION_PROFILING=0 /I%X% /I%X%\src /I%X%\third_party /I%X%\third_party\fmt\include ^
  "%~dp0ppc_oracle.cc" /link /LIBPATH:%X%\build\obj\Windows\Release xenia-cpu.lib xenia-base.lib fmt.lib ^
  /OUT:ppc_oracle.exe
