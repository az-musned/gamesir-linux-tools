@echo off
rem Build gsprobe.exe. Run from a "Developer Command Prompt for VS" (MSVC),
rem or with MinGW-w64's g++ on PATH.
cd /d "%~dp0"
where cl >nul 2>nul
if %errorlevel%==0 (
  cl /nologo /O2 /EHsc /std:c++17 /W4 gsprobe.cpp setupapi.lib hid.lib cfgmgr32.lib advapi32.lib /Fe:gsprobe.exe
  goto :eof
)
where g++ >nul 2>nul
if %errorlevel%==0 (
  g++ -O2 -std=c++17 -Wall -Wextra -static -o gsprobe.exe gsprobe.cpp -lsetupapi -lhid -lcfgmgr32 -ladvapi32
  goto :eof
)
echo No compiler found. Install "Build Tools for Visual Studio" (C++ workload) or MinGW-w64.
exit /b 1
