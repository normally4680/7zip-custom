@echo off
REM Launch 7zip-custom: close old instances first to guarantee the new build
taskkill /f /im 7zFM.exe >nul 2>&1
taskkill /f /im 7zG.exe >nul 2>&1
timeout /t 1 /nobreak >nul
start "" "%~dp07zFM.exe"
