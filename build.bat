@echo off
rem Lunhui Tower - Windows/MSVC build script
rem Usage: build.bat            -> build\mota.exe
rem        build.bat --selftest -> build and run self tests

setlocal
cd /d "%~dp0"
if not exist build mkdir build

cl /nologo /EHsc /utf-8 /std:c++17 /O2 /W3 /WX- ^
   /D_CRT_SECURE_NO_WARNINGS ^
   src\main.cpp src\game.cpp src\ui.cpp ^
   /Fe:build\mota.exe /Fo:build\ ^
   /link /SUBSYSTEM:CONSOLE

if errorlevel 1 (
  echo.
  echo [BUILD FAILED]
  exit /b 1
)

if not exist build\data xcopy /E /I /Q /Y data build\data >nul
echo.
echo [BUILD OK] build\mota.exe
if "%1"=="--selftest" (
  echo.
  build\mota.exe --selftest
  exit /b %errorlevel%
)
exit /b 0