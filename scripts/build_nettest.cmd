@echo off
REM Build dist\nettest.exe - headless multi-peer registry tracer (Phase 2, Plan
REM 01): host + 3 clients get unique PlayerIds, a 4th is cleanly rejected, a
REM freed slot is reused (src\nettest). Links the REAL src\plugin\net\NetLink.cpp
REM production code path (not a reimplemented ENet loop), plus the SAME
REM vendored+patched ENet sources and v100 (VC++ 2010) x64 toolchain the plugin
REM and tunneltest use.
setlocal

set "REPO=%~dp0.."
pushd "%REPO%" >nul
set "REPO=%CD%"
popd >nul

set "VS10=C:\Program Files (x86)\Microsoft Visual Studio 10.0"
set "VC=%VS10%\VC"
set "SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1"

set "PATH=%VC%\bin\amd64;%VC%\bin;%VS10%\Common7\IDE;%SDK%\Bin\x64;%SDK%\Bin;%PATH%"
set "INCLUDE=%VC%\include;%SDK%\Include;%REPO%\third_party\vc10_compat;%REPO%\third_party\enet\enet\include"
set "LIB=%VC%\lib\amd64;%SDK%\Lib\x64"

if not exist "%REPO%\dist" mkdir "%REPO%\dist"
if not exist "%REPO%\build\nettest" mkdir "%REPO%\build\nettest"

set "ENET=%REPO%\third_party\enet\enet"

echo === Building nettest.exe (Release^|x64, v100) ===
REM WIN32_LEAN_AND_MEAN matches KenshiCoop.vcxproj's project-wide define: it
REM excludes winsock.h from windows.h, so windows.h (pulled in by NetLink.h)
REM can coexist with enet's own winsock2.h include without a
REM "redefinition; different linkage" clash (C2375).
cl.exe /nologo /O2 /EHsc /W3 /DWIN32 /D WIN32_LEAN_AND_MEAN ^
    /Fo"%REPO%\build\nettest\\" ^
    /Fe"%REPO%\dist\nettest.exe" ^
    "%REPO%\src\nettest\main.cpp" ^
    "%REPO%\src\plugin\net\NetLink.cpp" ^
    "%ENET%\callbacks.c" "%ENET%\compress.c" "%ENET%\host.c" "%ENET%\list.c" ^
    "%ENET%\packet.c" "%ENET%\peer.c" "%ENET%\protocol.c" "%ENET%\win32.c" ^
    ws2_32.lib winmm.lib
if errorlevel 1 (
    echo nettest build FAILED
    exit /b 1
)
echo nettest built: %REPO%\dist\nettest.exe
exit /b 0
