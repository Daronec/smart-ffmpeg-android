@echo off
echo ========================================
echo Android NDK Setup for WSL2
echo ========================================
echo.

REM Find Android SDK location
set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"
if not exist "%ANDROID_SDK%" (
    set "ANDROID_SDK=%USERPROFILE%\AppData\Local\Android\Sdk"
)

if not exist "%ANDROID_SDK%" (
    echo ERROR: Android SDK not found!
    echo.
    echo Please install Android Studio and Android SDK first.
    echo Expected location: %LOCALAPPDATA%\Android\Sdk
    echo.
    pause
    exit /b 1
)

echo Android SDK found: %ANDROID_SDK%
echo.

REM Find NDK
echo Looking for NDK...
set "NDK_DIR=%ANDROID_SDK%\ndk"

if not exist "%NDK_DIR%" (
    echo ERROR: NDK directory not found!
    echo.
    echo Please install Android NDK:
    echo 1. Open Android Studio
    echo 2. Tools -^> SDK Manager
    echo 3. SDK Tools tab
    echo 4. Check "NDK (Side by side)"
    echo 5. Click Apply
    echo.
    pause
    exit /b 1
)

REM Find latest NDK version
for /f "delims=" %%i in ('dir /b /ad /o-n "%NDK_DIR%"') do (
    set "NDK_VERSION=%%i"
    goto :found_ndk
)

:found_ndk
if not defined NDK_VERSION (
    echo ERROR: No NDK version found in %NDK_DIR%
    echo.
    pause
    exit /b 1
)

set "ANDROID_NDK=%NDK_DIR%\%NDK_VERSION%"
echo NDK found: %ANDROID_NDK%
echo NDK version: %NDK_VERSION%
echo.

REM Convert Windows path to WSL path
for /f "tokens=*" %%i in ('wsl wslpath -a "%ANDROID_SDK%"') do set WSL_SDK_PATH=%%i
for /f "tokens=*" %%i in ('wsl wslpath -a "%ANDROID_NDK%"') do set WSL_NDK_PATH=%%i

echo WSL SDK path: %WSL_SDK_PATH%
echo WSL NDK path: %WSL_NDK_PATH%
echo.

REM Setup environment in WSL2
echo Setting up environment in WSL2...
wsl bash -c "echo 'export ANDROID_HOME=\"%WSL_SDK_PATH%\"' >> ~/.bashrc"
wsl bash -c "echo 'export ANDROID_NDK_HOME=\"%WSL_NDK_PATH%\"' >> ~/.bashrc"
wsl bash -c "echo 'export PATH=\$PATH:\$ANDROID_HOME/platform-tools' >> ~/.bashrc"

echo.
echo ========================================
echo Setup completed!
echo ========================================
echo.
echo Environment variables added to ~/.bashrc:
echo   ANDROID_HOME=%WSL_SDK_PATH%
echo   ANDROID_NDK_HOME=%WSL_NDK_PATH%
echo.
echo Now you can run: build_ffmpeg.bat
echo.
pause
