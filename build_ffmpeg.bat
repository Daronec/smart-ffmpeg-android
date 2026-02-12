@echo off
echo ========================================
echo Smart FFmpeg Android - Windows Build
echo ========================================
echo.

REM Check if WSL2 is installed
wsl --status >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: WSL2 is not installed!
    echo.
    echo Please install WSL2 first:
    echo 1. Open PowerShell as Administrator
    echo 2. Run: wsl --install
    echo 3. Restart computer
    echo 4. Run this script again
    echo.
    pause
    exit /b 1
)

echo WSL2 detected.
echo.

REM Find Android SDK
set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"
if not exist "%ANDROID_SDK%" (
    set "ANDROID_SDK=%USERPROFILE%\AppData\Local\Android\Sdk"
)

if not exist "%ANDROID_SDK%" (
    echo ERROR: Android SDK not found!
    echo Expected location: %LOCALAPPDATA%\Android\Sdk
    echo.
    echo Please install Android Studio and Android SDK.
    echo.
    pause
    exit /b 1
)

echo Android SDK found: %ANDROID_SDK%

REM Find NDK
set "NDK_DIR=%ANDROID_SDK%\ndk"
if not exist "%NDK_DIR%" (
    echo ERROR: NDK not found!
    echo.
    echo Please install Android NDK:
    echo 1. Open Android Studio
    echo 2. Tools -^> SDK Manager -^> SDK Tools
    echo 3. Check "NDK (Side by side)"
    echo 4. Click Apply
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
set "ANDROID_NDK=%NDK_DIR%\%NDK_VERSION%"
echo NDK found: %ANDROID_NDK%
echo NDK version: %NDK_VERSION%
echo.

REM Convert to WSL paths
for /f "tokens=*" %%i in ('wsl wslpath -a "%CD%"') do set WSL_PATH=%%i
for /f "tokens=*" %%i in ('wsl wslpath -a "%ANDROID_SDK%"') do set WSL_SDK_PATH=%%i
for /f "tokens=*" %%i in ('wsl wslpath -a "%ANDROID_NDK%"') do set WSL_NDK_PATH=%%i

echo Current directory: %CD%
echo WSL path: %WSL_PATH%
echo WSL NDK path: %WSL_NDK_PATH%
echo.

REM Run build script in WSL2 with NDK path
echo Running FFmpeg build script...
echo This will take 15-25 minutes...
echo.
wsl bash -c "export ANDROID_HOME='%WSL_SDK_PATH%' && export ANDROID_NDK_HOME='%WSL_NDK_PATH%' && cd '%WSL_PATH%' && chmod +x build_ffmpeg.sh && ./build_ffmpeg.sh"

if %errorlevel% equ 0 (
    echo.
    echo ========================================
    echo Build completed successfully!
    echo ========================================
    echo.
    echo Next steps:
    echo 1. Run: gradlew.bat assembleRelease
    echo 2. Check: build\outputs\aar\smart-ffmpeg-android-release.aar
    echo.
) else (
    echo.
    echo ========================================
    echo Build failed!
    echo ========================================
    echo.
    echo Please check the error messages above.
    echo.
)

pause
