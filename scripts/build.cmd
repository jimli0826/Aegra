@echo off
setlocal

set "AEGRA_CONFIGURATION=%~1"
if "%AEGRA_CONFIGURATION%"=="" set "AEGRA_CONFIGURATION=Debug"

if /I "%AEGRA_CONFIGURATION%"=="Debug" (
    set "AEGRA_PRESET=vs2026-debug"
) else if /I "%AEGRA_CONFIGURATION%"=="Release" (
    set "AEGRA_PRESET=vs2026-release"
) else (
    echo Unsupported configuration: %AEGRA_CONFIGURATION%
    exit /b 2
)

set "AEGRA_VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Insiders"
set "AEGRA_CMAKE=%AEGRA_VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "VCPKG_ROOT=%AEGRA_VS_ROOT%\VC\vcpkg"
set "VCPKG_VISUAL_STUDIO_PATH=%AEGRA_VS_ROOT%"

call "%AEGRA_VS_ROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

"%AEGRA_CMAKE%" --preset %AEGRA_PRESET% --no-warn-unused-cli
if errorlevel 1 exit /b %errorlevel%

"%AEGRA_CMAKE%" --build --preset %AEGRA_PRESET%
if errorlevel 1 exit /b %errorlevel%

"%AEGRA_CMAKE%" "-DAEGRA_SOURCE_ROOT=%~dp0.." -P "%~dp0..\cmake\CheckSourceLimits.cmake"
exit /b %errorlevel%
