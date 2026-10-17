@echo off
setlocal
rem ===========================================================================
rem  build.bat -- build audio_hook.dll (x64)
rem  Requires: Visual Studio Build Tools (MSVC) + Windows SDK
rem ===========================================================================

rem Locate VsDevCmd.bat via vswhere (Visual Studio 2017+ / Build Tools).
set "VSCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\Common7\Tools\VsDevCmd.bat" set "VSCMD=%%i\Common7\Tools\VsDevCmd.bat"
    )
)

if not defined VSCMD (
    echo [ERROR] Could not locate VsDevCmd.bat.
    echo         Install "Build Tools for Visual Studio" with the
    echo         "Desktop development with C++" workload, then retry.
    pause & exit /b 1
)

call "%VSCMD%" -arch=x64 -host_arch=x64 >nul
if not exist "hook_obj" mkdir "hook_obj"
cl -nologo -utf-8 -LD -MT -O2 -W3 -Iasiosdk -Fe:audio_hook.dll -Fo:hook_obj\ audio_hook.cpp -link -MACHINE:X64 user32.lib
if errorlevel 1 ( echo [FAILED] audio_hook.dll & pause & exit /b 1 )

cl -nologo -utf-8 -MT -O2 -W3 -Iasiosdk -Fe:asio_list.exe asio_list.cpp -link ole32.lib advapi32.lib user32.lib
if errorlevel 1 ( echo [FAILED] asio_list.exe & pause & exit /b 1 )

cl -nologo -utf-8 -MT -O2 -W3 -Fe:excl_rate_probe.exe excl_rate_probe.cpp -link ole32.lib uuid.lib
if errorlevel 1 ( echo [FAILED] excl_rate_probe.exe & pause & exit /b 1 )

cl -nologo -utf-8 -MT -O2 -W3 -Fe:session_tool.exe session_tool.cpp -link ole32.lib uuid.lib psapi.lib
if errorlevel 1 ( echo [FAILED] session_tool.exe & pause & exit /b 1 )

echo.
echo [DONE] audio_hook.dll + tools built.
echo Copy audio_hook.dll to ..\mods\Audio Hook\ and restart the game.
pause
endlocal
