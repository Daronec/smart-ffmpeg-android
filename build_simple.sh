#!/bin/bash

echo "========================================"
echo "Smart FFmpeg Android - Simple Build"
echo "========================================"
echo ""

# НАСТРОЙТЕ ЭТИ ПУТИ ПОД СЕБЯ:
# Замените "daron" на ваше имя пользователя Windows
ANDROID_SDK="/mnt/c/Users/daron/AppData/Local/Android/Sdk"
NDK_VERSION="29.0.13113456"  # Или другая версия из вашего SDK

# Проверка SDK
if [ ! -d "$ANDROID_SDK" ]; then
    echo "ERROR: Android SDK not found at: $ANDROID_SDK"
    echo ""
    echo "Пожалуйста, отредактируйте файл build_simple.sh"
    echo "и укажите правильный путь к Android SDK"
    echo ""
    echo "Чтобы найти путь, выполните в Windows CMD:"
    echo "  echo %LOCALAPPDATA%\\Android\\Sdk"
    echo ""
    echo "Затем конвертируйте путь для WSL:"
    echo "  C:\\Users\\USERNAME\\... -> /mnt/c/Users/USERNAME/..."
    exit 1
fi

echo "✓ Android SDK found: $ANDROID_SDK"

# Проверка NDK
ANDROID_NDK="$ANDROID_SDK/ndk/$NDK_VERSION"
if [ ! -d "$ANDROID_NDK" ]; then
    echo "ERROR: NDK not found at: $ANDROID_NDK"
    echo ""
    echo "Доступные версии NDK:"
    ls -1 "$ANDROID_SDK/ndk/" 2>/dev/null || echo "  (папка ndk не найдена)"
    echo ""
    echo "Отредактируйте build_simple.sh и укажите правильную версию NDK"
    exit 1
fi

echo "✓ Android NDK found: $ANDROID_NDK"
echo "  Version: $NDK_VERSION"
echo ""

# Установка переменных окружения
export ANDROID_HOME="$ANDROID_SDK"
export ANDROID_NDK_HOME="$ANDROID_NDK"

echo "Environment variables:"
echo "  ANDROID_HOME=$ANDROID_HOME"
echo "  ANDROID_NDK_HOME=$ANDROID_NDK_HOME"
echo ""

# Проверка build_ffmpeg.sh
if [ ! -f "build_ffmpeg.sh" ]; then
    echo "ERROR: build_ffmpeg.sh not found!"
    echo "Please run this script from smart-ffmpeg-android directory"
    exit 1
fi

# Сделать исполняемым
chmod +x build_ffmpeg.sh

# Запуск сборки
echo "========================================"
echo "Starting FFmpeg build..."
echo "This will take 15-25 minutes..."
echo "========================================"
echo ""

./build_ffmpeg.sh

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "✓ Build completed successfully!"
    echo "========================================"
    echo ""
    echo "Next steps:"
    echo "1. Exit WSL: exit"
    echo "2. In Windows CMD run: gradlew.bat assembleRelease"
    echo "3. Check: build\\outputs\\aar\\smart-ffmpeg-android-release.aar"
    echo ""
else
    echo ""
    echo "========================================"
    echo "✗ Build failed!"
    echo "========================================"
    echo ""
    echo "Please check the error messages above."
    exit 1
fi
