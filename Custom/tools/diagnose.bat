@echo off
REM ============================================================
REM  z7 看图/密码库 问题诊断
REM  双击 -> 自动打开 7zFM -> 复现一次问题 -> 关掉 7zFM 回到本窗口
REM  本窗口会把日志打出来，把内容发给我即可
REM ============================================================
setlocal
cd /d "%~dp0"

set "Z7CUSTOM_DEBUG=%~dp0hook.log"
if exist "%Z7CUSTOM_DEBUG%" del /q "%Z7CUSTOM_DEBUG%"

echo.
echo === 运行身份 ===
whoami
echo USERPROFILE=%USERPROFILE%
echo.

echo === 文件检查 ===
if exist "%~dp07zFM.exe"      (echo [ok] 7zFM.exe)        else (echo [MISSING] 7zFM.exe)
if exist "%~dp07z.exe"        (echo [ok] 7z.exe)          else (echo [MISSING] 7z.exe)
if exist "%~dp07z.dll"        (echo [ok] 7z.dll)          else (echo [MISSING] 7z.dll)
if exist "%~dp07zVaultUi.dll" (echo [ok] 7zVaultUi.dll)   else (echo [MISSING] 7zVaultUi.dll)

echo === 密码库是否可见 ===
if exist "%LOCALAPPDATA%\7zip-custom\vault.dat" (
    echo [ok] vault.dat 存在
) else (
    echo [MISSING] 当前上下文看不到密码库 -> 说明 7zFM 不是在你自己账户下
)
echo.

echo === 7z.exe 能否被启动 ===
start /wait "" "%~dp07z.exe" i >nul 2>&1
echo [7z.exe exit code] %ERRORLEVEL%  ^(0=ok^)

echo.
echo === 临时目录写入检查 ===
echo TEMP=%TEMP%
if not exist "%TEMP%\7zip-custom" mkdir "%TEMP%\7zip-custom" 2>nul
if exist "%TEMP%\7zip-custom" (
    echo [ok] 能在 TEMP 下创建 7zip-custom 目录
) else (
    echo [FAIL] 无法在 TEMP 下创建目录，错误码 %ERRORLEVEL%
)
echo.

echo 正在启动 7zFM.exe ... 请复现一次问题（例如双击压缩包里的图片）
start "" "%~dp07zFM.exe"
echo 复现完、把 7zFM 关掉后，回到本窗口按任意键
pause >nul

echo.
echo === hook 日志 (%Z7CUSTOM_DEBUG%) ===
if exist "%Z7CUSTOM_DEBUG%" (
    type "%Z7CUSTOM_DEBUG%"
) else (
    echo 没有日志 -> hook 没被调用，或者 7zFM 不是从本目录启动的
)
echo.
echo === MOTW check ===
powershell -NoProfile -Command "Get-ChildItem -Path '%~dp0' -File | ForEach-Object { if (Get-Item $_.FullName -Stream Zone.Identifier -ErrorAction SilentlyContinue) { 'MOTW: ' + $_.Name } }"
echo ^(empty = no MOTW^)

echo === 预览目录 ===
if exist "%TEMP%\7zip-custom\preview" dir /b "%TEMP%\7zip-custom\preview"
echo.
pause
