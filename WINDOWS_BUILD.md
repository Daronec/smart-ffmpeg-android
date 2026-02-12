# Building on Windows

This guide explains how to build smart-ffmpeg-android on Windows.

## Prerequisites

### 1. Install WSL2 (Recommended)

FFmpeg build requires Unix tools. WSL2 is the easiest solution.

```powershell
# In PowerShell (Admin)
wsl --install
# Restart computer
```

### 2. Install Ubuntu in WSL2

```bash
# In WSL2 Ubuntu terminal
sudo apt update
sudo apt install build-essential yasm nasm pkg-config wget git
```

### 3. Install Android Studio

Download from: https://developer.android.com/studio

### 4. Install Android NDK

In Android Studio:

- Tools → SDK Manager → SDK Tools
- Check "NDK (Side by side)"
- Click Apply

### 5. Set Environment Variables

```bash
# In WSL2, add to ~/.bashrc
export ANDROID_HOME=/mnt/c/Users/YOUR_USERNAME/AppData/Local/Android/Sdk
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/26.x.xxxx
export PATH=$PATH:$ANDROID_HOME/platform-tools
```

## Building

### Option 1: WSL2 (Recommended)

```bash
# In WSL2 terminal
cd /mnt/c/path/to/smart-ffmpeg-android

# Build FFmpeg
./build_ffmpeg.sh

# Build AAR
./gradlew assembleRelease
```

### Option 2: Native Windows (Advanced)

Requires:

- MSYS2 or Cygwin
- MinGW toolchain
- More complex setup

Not recommended for beginners.

## Common Issues

### WSL2 Path Issues

```bash
# Windows path: C:\Users\Name\project
# WSL2 path: /mnt/c/Users/Name/project

# Always use WSL2 paths in scripts
cd /mnt/c/Users/Name/smart-ffmpeg-android
```

### NDK Not Found

```bash
# Check NDK location
ls $ANDROID_NDK_HOME

# If not found, update path in ~/.bashrc
export ANDROID_NDK_HOME=/mnt/c/Users/YOUR_USERNAME/AppData/Local/Android/Sdk/ndk/26.x.xxxx
source ~/.bashrc
```

### Permission Issues

```bash
# Make scripts executable
chmod +x build_ffmpeg.sh
chmod +x gradlew
```

### Line Ending Issues

```bash
# Convert Windows line endings to Unix
sudo apt install dos2unix
dos2unix build_ffmpeg.sh
dos2unix gradlew
```

## Alternative: Docker

Use Docker to avoid WSL2 setup:

```dockerfile
# Dockerfile
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential yasm nasm pkg-config wget git \
    openjdk-11-jdk

# Add Android SDK/NDK
# ... (see Docker documentation)

WORKDIR /workspace
```

```powershell
# Build in Docker
docker build -t ffmpeg-builder .
docker run -v ${PWD}:/workspace ffmpeg-builder ./build_ffmpeg.sh
```

## Publishing from Windows

```powershell
# In PowerShell or WSL2
./gradlew publish
```

Credentials in `~/.gradle/gradle.properties`:

```properties
gpr.user=YOUR_GITHUB_USERNAME
gpr.key=YOUR_GITHUB_TOKEN
```

## IDE Setup

### Android Studio on Windows

1. Open project in Android Studio
2. Sync Gradle
3. Build → Make Project
4. Build → Build Bundle(s) / APK(s) → Build APK(s)

### VS Code on Windows

1. Install WSL extension
2. Open folder in WSL
3. Use integrated terminal (WSL)
4. Run build commands

## Tips

1. **Use WSL2** - Much easier than native Windows
2. **Keep paths short** - Avoid spaces in paths
3. **Use Unix line endings** - For shell scripts
4. **Check permissions** - chmod +x for scripts
5. **Use WSL paths** - /mnt/c/... not C:\...

## Troubleshooting

### "bash: ./build_ffmpeg.sh: /bin/bash^M: bad interpreter"

Line ending issue:

```bash
dos2unix build_ffmpeg.sh
```

### "NDK not found"

Check path:

```bash
echo $ANDROID_NDK_HOME
ls $ANDROID_NDK_HOME
```

### "Permission denied"

Make executable:

```bash
chmod +x build_ffmpeg.sh gradlew
```

### Gradle issues

```bash
# In WSL2
./gradlew clean
./gradlew assembleRelease --stacktrace
```

## Performance

WSL2 is fast enough for development:

- FFmpeg build: 15-25 minutes
- AAR build: 1-2 minutes

## Recommended Workflow

1. **Development**: Use Android Studio on Windows
2. **FFmpeg Build**: Use WSL2
3. **AAR Build**: Use WSL2 or Android Studio
4. **Testing**: Use Android Studio
5. **Publishing**: Use WSL2

## Resources

- WSL2 Documentation: https://docs.microsoft.com/en-us/windows/wsl/
- Android NDK: https://developer.android.com/ndk
- FFmpeg: https://ffmpeg.org/

## Support

If you encounter Windows-specific issues:

1. Check this guide
2. Search GitHub Issues
3. Ask in Discussions
4. Create new Issue with "Windows" label
