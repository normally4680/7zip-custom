@echo off
REM ============================================================
REM  7-Zip custom build script  (MSVC / nmake)
REM
REM  Usage:  build-msvc.bat [target]
REM    7z    -> CPP\7zip\UI\Console         (7z.exe)
REM    7zg   -> CPP\7zip\UI\GUI             (7zG.exe)
REM    7zdll -> CPP\7zip\Bundles\Format7zF  (7z.dll)
REM    7zfm  -> CPP\7zip\Bundles\Fm         (7zFM.exe)
REM    7zshl -> CPP\7zip\UI\Explorer        (7-zip.dll shell extension)
REM    7za   -> CPP\7zip\Bundles\Alone      (7za.exe)
REM    7zz   -> CPP\7zip\Bundles\Alone2     (7zz.exe)
REM    clean -> remove all intermediate x64 dirs
REM
REM  No argument: build 7z.dll + 7z.exe + 7zG.exe + 7zFM.exe
REM
REM  NOTE: keep this file ASCII-only and CRLF. cmd.exe mis-parses
REM        UTF-8 comments inside batch files.
REM ============================================================
setlocal EnableDelayedExpansion

REM Locate Visual Studio: use VSDIR if set, otherwise the newest VS install.
REM Local dev machines may also have a fixed install path; CI uses vswhere.
if not defined VSDIR if exist "D:\Microsoft Visual Studio\18\Community" set "VSDIR=D:\Microsoft Visual Studio\18\Community"
if not defined VSDIR (
    for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSDIR=%%I"
)
set "VCVARS=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat"

if not exist "!VCVARS!" (
    echo [ERROR] vcvars64.bat not found. Set VSDIR to your Visual Studio directory.
    exit /b 1
)

call "!VCVARS!" >nul
if errorlevel 1 ( echo [ERROR] vcvars64 init failed & exit /b 1 )

REM ---- Windows SDK fallback ----
REM vcvars detects the SDK through the registry. If that detection fails,
REM INCLUDE/LIB miss the ucrt/um paths and cl reports "cannot open stddef.h".
REM Detect the newest SDK by scanning Windows Kits\Include instead.
if "%WindowsSDKVersion%"=="" goto :sdk_fix
if "%WindowsSDKVersion%"=="\" goto :sdk_fix
goto :sdk_ok
:sdk_fix
set "KITS=C:\Program Files (x86)\Windows Kits\10"
set "SDKV="
for /f "delims=" %%V in ('dir /b /ad /o-n "%KITS%\Include"') do if not defined SDKV set "SDKV=%%V"
if not defined SDKV ( echo [ERROR] Windows SDK not found under "%KITS%" & exit /b 1 )
set "WindowsSDKVersion=!SDKV!\"
set "INCLUDE=!INCLUDE!;!KITS!\Include\!SDKV!\ucrt;!KITS!\Include\!SDKV!\um;!KITS!\Include\!SDKV!\shared;!KITS!\Include\!SDKV!\winrt;!KITS!\Include\!SDKV!\cppwinrt"
set "LIB=!LIB!;!KITS!\Lib\!SDKV!\ucrt\x64;!KITS!\Lib\!SDKV!\um\x64"
set "PATH=!PATH!;!KITS!\bin\!SDKV!\x64"
echo [ENV] Windows SDK patched manually: !SDKV!
:sdk_ok

echo [ENV] MSVC %VCToolsVersion%  Arch=%VSCMD_ARG_TGT_ARCH%  SDK=%WindowsSDKVersion%
echo [ENV] LIB=%LIB%
echo.

if /i "%~1"=="clean" goto :clean
if "%~1"=="" ( set "LIST=custom 7zdll 7z 7zg 7zfm 7zshl vault" ) else (
    if /i "%~1"=="custom" ( set "LIST=%*" ) else ( set "LIST=custom %*" )
)

set "FAILED="
for %%T in (%LIST%) do (
    call :build %%T
    if errorlevel 1 goto :stopped
)
echo.
if defined FAILED ( echo [FAIL] failed targets:%FAILED% & exit /b 1 )
call :stage
if errorlevel 1 exit /b 1
echo [ALL DONE]
exit /b 0

REM ---- collect built binaries into _out\x64 for easy run / debug ----
REM Every copy is checked: if the destination exe is running, "copy /y" fails
REM and would leave a stale binary in _out while the build still reports OK.
:stage
set "OUT=%~dp0_out\x64"
if not exist "%OUT%" mkdir "%OUT%"
set "STAGEFAIL="
call :stagecopy "%~dp0CPP\7zip\Bundles\Format7zF\x64\7z.dll"  "%OUT%\"
call :stagecopy "%~dp0CPP\7zip\UI\Console\x64\7z.exe"        "%OUT%\"
call :stagecopy "%~dp0CPP\7zip\UI\GUI\x64\7zG.exe"           "%OUT%\"
call :stagecopy "%~dp0CPP\7zip\Bundles\Fm\x64\7zFM.exe"      "%OUT%\"
call :stagecopy "%~dp0CPP\7zip\UI\Explorer\x64\7-zip.dll"    "%OUT%\"
if not exist "%OUT%\Lang" mkdir "%OUT%\Lang"
copy /y "%~dp0Custom\lang\*.txt" "%OUT%\Lang\" >nul
copy /y "%~dp0Custom\tools\diagnose.bat" "%OUT%\diagnose.bat" >nul
copy /y "%~dp0Custom\tools\run.bat" "%OUT%\run.bat" >nul
if defined STAGEFAIL (
    echo [FAIL] stage copy failed - close 7zFM/7zG/7z.exe/7z.dll users and rebuild
    exit /b 1
)
echo [STAGE] output dir: %OUT%
exit /b 0

:stagecopy
REM %1 = source, %2 = destination dir (with trailing backslash)
if not exist "%~1" exit /b 0
copy /y "%~1" "%~2" >nul 2>nul
if errorlevel 1 (
    echo [ERROR] cannot update "%~nx1" - file is running or locked
    set "STAGEFAIL=1"
)
exit /b 0

:build
set "NAME="
set "DIR="
set "MK="
if /i "%~1"=="7z"     ( set "NAME=7z.exe"      & set "DIR=CPP\7zip\UI\Console" )
if /i "%~1"=="7zg"    ( set "NAME=7zG.exe"     & set "DIR=CPP\7zip\UI\GUI" )
if /i "%~1"=="7zdll"  ( set "NAME=7z.dll"      & set "DIR=CPP\7zip\Bundles\Format7zF" )
if /i "%~1"=="7zfm"   ( set "NAME=7zFM.exe"    & set "DIR=CPP\7zip\Bundles\Fm" )
if /i "%~1"=="7zshl"  ( set "NAME=7-zip.dll"   & set "DIR=CPP\7zip\UI\Explorer" )
if /i "%~1"=="7za"    ( set "NAME=7za.exe"     & set "DIR=CPP\7zip\Bundles\Alone" )
if /i "%~1"=="7zz"    ( set "NAME=7zz.exe"     & set "DIR=CPP\7zip\Bundles\Alone2" )
if /i "%~1"=="custom" ( set "NAME=Custom.lib"  & set "DIR=Custom" & set "MK=lib" )
if /i "%~1"=="vault"  ( set "NAME=7zVault.exe" & set "DIR=Custom" & set "MK=all" )
if not defined NAME ( echo [SKIP] unknown target: %~1 & exit /b 0 )

echo ===== build %NAME%  (%DIR%) =====
REM Force relink: nmake does not track Custom.lib as a link dependency,
REM so a changed overlay lib would otherwise be ignored.
if not defined MK if exist "%~dp0%DIR%\x64\%NAME%" del /q "%~dp0%DIR%\x64\%NAME%"
pushd "%~dp0%DIR%"
if defined MK (
    nmake /nologo !MK!
) else (
    nmake /nologo NEW_COMPILER=1 MY_STATIC_LINK=1
)
set "RC=!ERRORLEVEL!"
popd
if not "!RC!"=="0" (
    echo [FAIL] %NAME%  code=!RC!
    set "FAILED=!FAILED! %~1"
    exit /b 1
)
echo [OK] %NAME%
echo.
exit /b 0

:stopped
echo [STOP] a target failed, aborting
exit /b 1

:clean
for %%D in (CPP\7zip\UI\Console CPP\7zip\UI\GUI CPP\7zip\UI\FileManager CPP\7zip\Bundles\Format7zF CPP\7zip\Bundles\Fm CPP\7zip\Bundles\Alone CPP\7zip\Bundles\Alone2) do (
    if exist "%~dp0%%D\x64" rd /s /q "%~dp0%%D\x64"
    if exist "%~dp0%%D\b"   rd /s /q "%~dp0%%D\b"
)
echo [DONE] intermediate dirs removed
exit /b 0
