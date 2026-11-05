@echo off
rem Builds the oracles against the xenia-canary built in ..\..\..\xenia-canary
rem (Release, MSVC: its xenia-cpu, xenia-base, mspack and fmt libraries), into
rem ..\..\..\external\oracle (XENIA_CANARY and ORACLE_OUT name others, e.g.
rem from a worktree). Not part of XenoForge's build: oracles for checking it.
rem ppc_oracle: the PPC decoder (docs/recompiler.md, "Checks"); lzx_oracle:
rem an executable's image unpacked from normal (LZX) compression.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set X=%~dp0..\..\..\xenia-canary
if defined XENIA_CANARY set X=%XENIA_CANARY%
set OUT=%~dp0..\..\..\external\oracle
if defined ORACLE_OUT set OUT=%ORACLE_OUT%
if not exist "%OUT%" mkdir "%OUT%"
cd /d "%OUT%"
set CFLAGS=/nologo /O2 /MD /EHsc /std:c++20 /utf-8 /DNDEBUG /DWIN32 /D_WIN64=1 /D_AMD64=1 /DUNICODE /D_UNICODE ^
  /D_LIB /DXE_OPTION_PROFILING=0 /I%X% /I%X%\src /I%X%\third_party /I%X%\third_party\fmt\include
cl %CFLAGS% "%~dp0ppc_oracle.cc" /link /LIBPATH:%X%\build\obj\Windows\Release xenia-cpu.lib xenia-base.lib ^
  fmt.lib /OUT:ppc_oracle.exe || exit /b 1
cl %CFLAGS% "%~dp0lzx_oracle.cc" /link /LIBPATH:%X%\build\obj\Windows\Release xenia-cpu.lib mspack.lib ^
  xenia-base.lib fmt.lib shell32.lib advapi32.lib user32.lib ole32.lib /OUT:lzx_oracle.exe || exit /b 1
