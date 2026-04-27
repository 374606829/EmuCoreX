@echo off
chcp 65001 >nul
echo ========================================
echo   EmuCoreX 源码同步到 GitHub
echo   仓库: https://github.com/374606829/Emucorex-online
echo   目录: 本脚本所在文件夹（仅本仓库变更）
echo ========================================
echo.

:: 切换到本脚本所在目录（EmuCoreX 工程根目录）
cd /d "%~dp0"

:: 检查是否在 Git 仓库中
git rev-parse --is-inside-work-tree >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [错误] 当前目录不是一个 Git 仓库，请先在此目录执行 git init 并配置 remote。
    pause
    exit /b 1
)

:: 获取当前日期时间作为默认提交信息
for /f "tokens=1-3 delims=/ " %%a in ('date /t') do set DATE=%%a-%%b-%%c
for /f "tokens=1-2 delims=: " %%a in ('time /t') do set TIME=%%a:%%b
set DEFAULT_MSG=update: 同步 EmuCoreX 源码 %DATE% %TIME%

:: 允许用户输入自定义提交信息（直接回车则使用默认信息）
echo [提示] 请输入提交信息（直接回车使用默认信息）:
echo        默认: %DEFAULT_MSG%
set /p COMMIT_MSG="> "
if "%COMMIT_MSG%"=="" set COMMIT_MSG=%DEFAULT_MSG%

echo.
echo [1/4] 检查变更状态...
git status --short
echo.

:: 检查是否有变更
git diff --quiet --exit-code >nul 2>&1
set DIFF_UNSTAGED=%ERRORLEVEL%
git diff --quiet --cached --exit-code >nul 2>&1
set DIFF_STAGED=%ERRORLEVEL%

:: 检查是否有未跟踪的文件
for /f %%i in ('git ls-files --others --exclude-standard') do (
    set HAS_UNTRACKED=1
    goto :check_done
)
set HAS_UNTRACKED=0
:check_done

if %DIFF_UNSTAGED% equ 0 if %DIFF_STAGED% equ 0 if "%HAS_UNTRACKED%"=="0" (
    echo [提示] 没有检测到任何变更，无需提交。
    pause
    exit /b 0
)

echo [2/4] 添加所有变更到暂存区（遵守 .gitignore，仅本仓库）...
git add .

echo [3/4] 提交变更: %COMMIT_MSG%
git commit -m "%COMMIT_MSG%"

if %ERRORLEVEL% neq 0 (
    echo [错误] 提交失败，请检查是否有变更需要提交。
    pause
    exit /b 1
)

echo [4/4] 推送到远程仓库 (origin/main)...
git push origin main

if %ERRORLEVEL% equ 0 (
    echo.
    echo ========================================
    echo   同步成功！源码已推送到 Emucorex-online。
    echo ========================================
) else (
    echo.
    echo ========================================
    echo   推送失败！请检查:
    echo   1. 网络连接是否正常
    echo   2. GitHub 凭证是否已配置
    echo   3. 是否有远程冲突（可尝试先 git pull）
    echo ========================================
)

echo.
pause
