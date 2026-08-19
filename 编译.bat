@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 936 >nul
cd /d "%~dp0"

rem ============================================================================
rem  AnyLink Secure Client - Windows build script
rem  Qt:    E:\Qt\6.11.1\mingw_64
rem  MinGW: E:\Qt\Tools\mingw1310_64
rem
rem  Usage:
rem    build.bat              Release + deploy + sslcon
rem    build.bat debug        Debug build
rem    build.bat clean        Remove build/out
rem    build.bat rebuild      Clean then build
rem    build.bat nodeploy     Build only
rem    build.bat nosslcon     Skip sslcon download
rem ============================================================================

if not defined QTDIR set "QTDIR=E:\Qt\6.11.1\mingw_64"
if not defined MINGW_DIR set "MINGW_DIR=E:\Qt\Tools\mingw1310_64"

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "BUILD_TYPE=release"
set "DO_CLEAN=0"
set "DO_BUILD=1"
set "DO_DEPLOY=1"
set "DO_SSLCON=1"
set "PAUSE_AT_END=1"

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="help" goto show_help
if /i "%~1"=="/?" goto show_help
if /i "%~1"=="-h" goto show_help
if /i "%~1"=="debug" set "BUILD_TYPE=debug"
if /i "%~1"=="release" set "BUILD_TYPE=release"
if /i "%~1"=="clean" (
    set "DO_CLEAN=1"
    set "DO_BUILD=0"
    set "DO_DEPLOY=0"
    set "DO_SSLCON=0"
)
if /i "%~1"=="rebuild" set "DO_CLEAN=1"
if /i "%~1"=="nodeploy" (
    set "DO_DEPLOY=0"
    set "DO_SSLCON=0"
)
if /i "%~1"=="nosslcon" set "DO_SSLCON=0"
if /i "%~1"=="nopause" set "PAUSE_AT_END=0"
shift
goto parse_args

:args_done
echo.
echo ========================================
echo   AnyLink Secure Client  Windows build
echo ========================================
echo.

if "%DO_CLEAN%"=="1" call :do_clean
if "%DO_BUILD%"=="0" goto finish_ok

rem Init git submodules BEFORE putting MinGW in PATH
call :init_submodules
if errorlevel 1 goto fail

call :setup_env
if errorlevel 1 goto fail

call :run_qmake
if errorlevel 1 goto fail

call :run_build
if errorlevel 1 goto fail

if "%DO_DEPLOY%"=="1" (
    call :run_deploy
    if errorlevel 1 goto fail
)

if "%DO_SSLCON%"=="1" (
    call :fetch_sslcon
    if errorlevel 1 goto fail
)

echo.
echo ----------------------------------------
echo Build OK
echo Out dir: %ROOT%\out\bin
echo Exe:     %ROOT%\out\bin\anylink.exe
echo ----------------------------------------
goto finish_ok

:show_help
echo AnyLink Secure Client - Windows build script
echo.
echo Usage: compile.bat [options]
echo.
echo   debug       Debug build
echo   release     Release build (default)
echo   clean       Remove build / out
echo   rebuild     Clean then build
echo   nodeploy    Build only, no runtime deploy
echo   nosslcon    Do not download vpnagent / sslcon / wintun
echo   nopause     Do not pause at the end
echo.
goto finish_ok

:do_clean
echo [clean] remove build and out
if exist "%ROOT%\build" rmdir /s /q "%ROOT%\build"
if exist "%ROOT%\out" rmdir /s /q "%ROOT%\out"
if exist "%ROOT%\Makefile" del /q "%ROOT%\Makefile" "%ROOT%\Makefile.Release" "%ROOT%\Makefile.Debug" 2>nul
if exist "%ROOT%\sslcon-windows10-amd64.7z" del /q "%ROOT%\sslcon-windows10-amd64.7z"
echo [clean] done
echo.
exit /b 0

:setup_env
if not exist "%QTDIR%\bin\qmake.exe" (
    echo [ERROR] qmake not found: %QTDIR%\bin\qmake.exe
    echo         Set QTDIR to your Qt kit directory.
    exit /b 1
)
if not exist "%MINGW_DIR%\bin\g++.exe" (
    echo [ERROR] g++ not found: %MINGW_DIR%\bin\g++.exe
    echo         Set MINGW_DIR to your MinGW directory.
    exit /b 1
)
if not exist "%MINGW_DIR%\bin\mingw32-make.exe" (
    echo [ERROR] mingw32-make not found: %MINGW_DIR%\bin\mingw32-make.exe
    exit /b 1
)

set "PATH=%MINGW_DIR%\bin;%QTDIR%\bin;%PATH%"
echo [Qt]    %QTDIR%
echo [MinGW] %MINGW_DIR%
qmake -v
echo.

if not exist "%QTDIR%\bin\Qt6WebSockets.dll" if not exist "%QTDIR%\lib\libQt6WebSockets.a" (
    echo [ERROR] Qt WebSockets module is missing.
    echo         anylink.pro requires: QT += websockets
    echo.
    echo         Open Qt Maintenance Tool and add the module:
    echo           E:\Qt\MaintenanceTool.exe
    echo         Qt 6.11.1 -^> Additional Libraries -^> Qt WebSockets
    echo         Then run this script again.
    exit /b 1
)

where g++.exe >nul 2>&1
if errorlevel 1 (
    echo [ERROR] g++.exe not in PATH
    exit /b 1
)
exit /b 0

:init_submodules
if exist "%ROOT%\3rdparty\SingleApplication\singleapplication.pri" (
    if exist "%ROOT%\3rdparty\qtkeychain\qtkeychain.pri" (
        echo [dep] 3rdparty submodules ready
        exit /b 0
    )
)

set "GIT="
if exist "%ProgramFiles%\Git\cmd\git.exe" set "GIT=%ProgramFiles%\Git\cmd\git.exe"
if not defined GIT (
    where git.exe >nul 2>&1
    if not errorlevel 1 for /f "delims=" %%i in ('where git.exe') do if not defined GIT set "GIT=%%i"
)
if not defined GIT (
    echo [ERROR] git.exe not found, cannot fetch SingleApplication / qtkeychain
    exit /b 1
)

rem Avoid Git-for-Windows "no working tree" when MinGW/MSYS env leaks in
set "GIT_DIR="
set "GIT_WORK_TREE="
set "GIT_COMMON_DIR="
set "MSYSTEM="

echo [dep] git submodule update --init --recursive
"%GIT%" -C "%ROOT%" submodule update --init --recursive
if errorlevel 1 (
    echo [dep] submodule failed, try git clone ...
    call :clone_dep "3rdparty\SingleApplication" "https://github.com/itay-grudev/SingleApplication.git" "master"
    if errorlevel 1 exit /b 1
    call :clone_dep "3rdparty\qtkeychain" "https://github.com/frankosterfeld/qtkeychain.git" "main"
    if errorlevel 1 exit /b 1
)

if not exist "%ROOT%\3rdparty\SingleApplication\singleapplication.pri" (
    echo [ERROR] missing 3rdparty\SingleApplication
    exit /b 1
)
if not exist "%ROOT%\3rdparty\qtkeychain\qtkeychain.pri" (
    echo [ERROR] missing 3rdparty\qtkeychain
    exit /b 1
)
echo [dep] submodules ready
exit /b 0

:clone_dep
set "DEP_PATH=%ROOT%\%~1"
set "DEP_URL=%~2"
set "DEP_BRANCH=%~3"
if exist "%DEP_PATH%\.git" (
    "%GIT%" -C "%DEP_PATH%" pull --ff-only
    exit /b 0
)
if exist "%DEP_PATH%" rmdir /s /q "%DEP_PATH%"
"%GIT%" clone --depth 1 --branch "%DEP_BRANCH%" "%DEP_URL%" "%DEP_PATH%"
if errorlevel 1 (
    echo [ERROR] git clone failed: %DEP_URL%
    exit /b 1
)
exit /b 0

:run_qmake
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
pushd "%ROOT%\build"

echo.
echo [qmake] generate Makefile  (%BUILD_TYPE%, win32-g++)
qmake "%ROOT%\anylink.pro" -spec win32-g++ "CONFIG+=%BUILD_TYPE%"
if errorlevel 1 (
    popd
    echo [ERROR] qmake failed
    exit /b 1
)
popd
exit /b 0

:run_build
pushd "%ROOT%\build"
echo.
echo [build] %BUILD_TYPE% ...

if /i "%BUILD_TYPE%"=="debug" (
    set "MAKEFILE=Makefile.Debug"
) else (
    set "MAKEFILE=Makefile.Release"
)

if exist "!MAKEFILE!" (
    mingw32-make -j %NUMBER_OF_PROCESSORS% -f !MAKEFILE!
) else (
    mingw32-make -j %NUMBER_OF_PROCESSORS%
)

if errorlevel 1 (
    popd
    echo [ERROR] compile failed
    exit /b 1
)
popd

if not exist "%ROOT%\out\bin\anylink.exe" (
    echo [ERROR] out\bin\anylink.exe was not generated
    exit /b 1
)
echo [build] anylink.exe generated
exit /b 0

:run_deploy
echo.
echo [deploy] windeployqt ...
windeployqt "%ROOT%\out\bin\anylink.exe" --no-translations --no-system-d3d-compiler --no-opengl-sw --no-svg
if errorlevel 1 (
    echo [ERROR] windeployqt failed
    exit /b 1
)

for %%F in (libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll) do (
    if exist "%MINGW_DIR%\bin\%%F" copy /y "%MINGW_DIR%\bin\%%F" "%ROOT%\out\bin\" >nul
)
echo [deploy] Qt / MinGW runtime copied to out\bin
exit /b 0

:fetch_sslcon
echo.
echo [sslcon] download vpnagent / sslcon / wintun ...

set "SSLCON_URL=https://github.com/tlslink/sslcon/releases/download/latest/sslcon-windows10-amd64.7z"
set "SSLCON_7Z=%ROOT%\out\sslcon-windows10-amd64.7z"
if not exist "%ROOT%\out" mkdir "%ROOT%\out"

where curl.exe >nul 2>&1
if errorlevel 1 (
    echo [WARN] curl.exe not found, skip sslcon
    echo        Download and extract vpnagent.exe sslcon.exe wintun.dll into out\bin
    echo        %SSLCON_URL%
    exit /b 0
)

curl.exe -k -L --fail -o "%SSLCON_7Z%" "%SSLCON_URL%"
if errorlevel 1 (
    echo [WARN] sslcon download failed
    echo        %SSLCON_URL%
    exit /b 0
)

set "SEVENZ="
if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZ=%ProgramFiles%\7-Zip\7z.exe"
if not defined SEVENZ if exist "!ProgramFiles(x86)!\7-Zip\7z.exe" set "SEVENZ=!ProgramFiles(x86)!\7-Zip\7z.exe"
if not defined SEVENZ (
    where 7z.exe >nul 2>&1
    if not errorlevel 1 set "SEVENZ=7z.exe"
)

if not defined SEVENZ (
    echo [WARN] 7z.exe not found, downloaded but not extracted: %SSLCON_7Z%
    echo        Install 7-Zip then re-run, or extract into out\bin manually.
    exit /b 0
)

"%SEVENZ%" x -y "-o%ROOT%\out" "%SSLCON_7Z%" >nul
copy /y "%ROOT%\out\vpnagent.exe" "%ROOT%\out\bin\" >nul 2>&1
copy /y "%ROOT%\out\sslcon.exe" "%ROOT%\out\bin\" >nul 2>&1
copy /y "%ROOT%\out\wintun.dll" "%ROOT%\out\bin\" >nul 2>&1

if exist "%ROOT%\out\bin\vpnagent.exe" (
    echo [sslcon] copied vpnagent.exe / sslcon.exe / wintun.dll to out\bin
) else (
    echo [WARN] vpnagent.exe not found after extract, check %SSLCON_7Z%
)
del /q "%SSLCON_7Z%" 2>nul
exit /b 0

:fail
echo.
echo Build FAILED.
if "%PAUSE_AT_END%"=="1" pause
endlocal
exit /b 1

:finish_ok
echo.
if "%PAUSE_AT_END%"=="1" pause
endlocal
exit /b 0
