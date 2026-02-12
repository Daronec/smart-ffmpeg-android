#!/bin/bash

echo "========================================"
echo "Smart FFmpeg Android - WSL2 Build"
echo "========================================"
echo ""

# Find Android SDK in Windows
# Try multiple possible locations
WINDOWS_SDK=""

# Get Windows username from path
WIN_USER=$(cmd.exe /c "echo %USERNAME%" 2>/dev/null | tr -d '\r')

echo "Detected Windows username: $WIN_USER"
echo "Looking for Android SDK..."
echo ""

# Try different possible locations
POSSIBLE_PATHS=(
    "/mnt/c/Users/$WIN_USER/AppData/Local/Android/Sdk"
    "/mnt/c/Users/daron/AppData/Local/Android/Sdk"
    "/mnt/c/Android/Sdk"
    "/mnt/c/Program Files/Android/Sdk"
)

for path in "${POSSIBLE_PATHS[@]}"; do
    echo "Checking: $path"
    if [ -d "$path" ]; then
        WINDOWS_SDK="$path"
        echo "✓ Found!"
        break
    else
        echo "✗ Not found"
    fi
done

if [ -z "$WINDOWS_SDK" ]; then
    echo ""
    echo "ERROR: Android SDK not found!"
    echo ""
    echo "Tried locations:"
    for path in "${POSSIBLE_PATHS[@]}"; do
        echo "  - $path"
    done
    echo ""
    echo "Please specify the correct path manually:"
    echo "  export ANDROID_HOME=/mnt/c/path/to/Android/Sdk"
    echo "  ./build_ffmpeg_wsl.sh"
    exit 1
fi

echo "Android SDK found: $WINDOWS_SDK"

# Find NDK
NDK_DIR="$WINDOWS_SDK/ndk"
if [ ! -d "$NDK_DIR" ]; then
    echo "ERROR: NDK directory not found!"
    echo ""
    echo "Please install Android NDK:"
    echo "1. Open Android Studio"
    echo "2. Tools -> SDK Manager -> SDK Tools"
    echo "3. Check 'NDK (Side by side)'"
    echo "4. Click Apply"
    exit 1
fi

# Find latest NDK version
NDK_VERSION=$(ls -1 "$NDK_DIR" | sort -V | tail -n 1)
if [ -z "$NDK_VERSION" ]; then
    echo "ERROR: No NDK version found in $NDK_DIR"
    exit 1
fi

ANDROID_NDK_HOME="$NDK_DIR/$NDK_VERSION"
echo "NDK found: $ANDROID_NDK_HOME"
echo "NDK version: $NDK_VERSION"
echo ""

# Export environment variables
export ANDROID_HOME="$WINDOWS_SDK"
export ANDROID_NDK_HOME="$ANDROID_NDK_HOME"

echo "Environment variables set:"
echo "  ANDROID_HOME=$ANDROID_HOME"
echo "  ANDROID_NDK_HOME=$ANDROID_NDK_HOME"
echo ""

# Check if build_ffmpeg.sh exists
if [ ! -f "build_ffmpeg.sh" ]; then
    echo "ERROR: build_ffmpeg.sh not found!"
    echo "Please run this script from smart-ffmpeg-android directory"
    exit 1
fi

# Make executable
chmod +x build_ffmpeg.sh

# Run build script
echo "Starting FFmpeg build..."
echo "This will take 15-25 minutes..."
echo ""

./build_ffmpeg.sh

if [ $? -eq 0 ]; then
    echo ""
    echo "========================================"
    echo "Build completed successfully!"
    echo "========================================"
    echo ""
    echo "Next steps:"
    echo "1. Exit WSL: exit"
    echo "2. Run: gradlew.bat assembleRelease"
    echo "3. Check: build\\outputs\\aar\\smart-ffmpeg-android-release.aar"
else
    echo ""
    echo "========================================"
    echo "Build failed!"
    echo "========================================"
    echo ""
    echo "Please check the error messages above."
    exit 1
fi
