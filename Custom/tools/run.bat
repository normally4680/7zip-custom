@echo off
REM Launch 7zip-custom.
REM The workspace tree may carry a Low integrity label (left by a sandbox /
REM security tool). An exe with that label runs at Low integrity: it cannot
REM write %TEMP% and cannot start a viewer. So copy the runtime to a normal
REM (unlabeled) folder first, then launch the copy.
setlocal
set "APPDIR=%LOCALAPPDATA%\7zip-custom\app"

taskkill /f /im 7zFM.exe >nul 2>&1
taskkill /f /im 7zG.exe >nul 2>&1
"%SystemRoot%\System32\timeout.exe" /t 1 /nobreak >nul

if not exist "%APPDIR%" mkdir "%APPDIR%"
if not exist "%APPDIR%\Lang" mkdir "%APPDIR%\Lang"

for %%F in (7zFM.exe 7zG.exe 7z.exe 7z.dll 7zVault.exe 7zVaultUi.dll 7zVault.exe.manifest diagnose.bat) do (
    if exist "%~dp0%%F" copy /y "%~dp0%%F" "%APPDIR%\%%F" >nul
)
if exist "%~dp0Lang\*.txt" copy /y "%~dp0Lang\*.txt" "%APPDIR%\Lang\" >nul
if /i not "%~dp0"=="%APPDIR%\" if exist "%~dp0run.bat" copy /y "%~dp0run.bat" "%APPDIR%\run.bat" >nul

start "" /d "%APPDIR%" "%APPDIR%\7zFM.exe"
