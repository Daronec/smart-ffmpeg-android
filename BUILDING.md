# Building Smart FFmpeg Android Library

## Prerequisites

### Required Tools

1. **Android NDK r26+**

   ```bash
   # Install via Android Studio SDK Manager or:
   export ANDROID_NDK_HOME=/path/to/ndk
   ```

2. **Build Tools** (Linux/macOS)

   ```bash
   # Ubuntu/Debian
   sudo apt-get install build-essential yasm nasm pkg-config wget

   # macOS
   brew install yasm nasm pkg-config wget
   ```

3. **Java 11+**
   ```bash
   java -version
   ```

## Building FFmpeg

### Option 1: Using Build Script (Recommended)

```bash
chmod +x build_ffmpeg.sh
./build_ffmpeg.sh
```

This will:

- Download FFmpeg 6.1
- Build for arm64-v8a and armeabi-v7a
- Install libraries to `src/main/jniLibs/`
- Copy headers to `src/main/cpp/include/`

### Option 2: Manual Build

See detailed instructions in `README.md`

## Building the AAR

```bash
# Clean build
./gradlew clean

# Build release AAR
./gradlew assembleRelease

# Check output
ls -lh build/outputs/aar/
```

Expected output: `smart-ffmpeg-android-release.aar` (~10-15MB)

## Verifying the Build

### Check AAR Contents

```bash
unzip -l build/outputs/aar/smart-ffmpeg-android-release.aar
```

Should contain:

- `jni/arm64-v8a/libffmpeg_bridge.so`
- `jni/arm64-v8a/libavcodec.so`
- `jni/arm64-v8a/libavformat.so`
- `jni/arm64-v8a/libavutil.so`
- `jni/arm64-v8a/libswscale.so`
- Same for `armeabi-v7a`
- `classes.jar` (Kotlin bridge classes)

### Check Size

```bash
du -h build/outputs/aar/smart-ffmpeg-android-release.aar
```

Should be under 15MB. If larger:

1. Verify FFmpeg configure flags
2. Check for unnecessary codecs/formats
3. Ensure `--enable-small` is used

### Test the Library

```bash
# Extract AAR
unzip build/outputs/aar/smart-ffmpeg-android-release.aar -d test_aar

# Check symbols
nm test_aar/jni/arm64-v8a/libffmpeg_bridge.so | grep Java_com_smartmedia
```

Should show:

- `Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_extractThumbnail`
- `Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoDuration`
- `Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadata`
- `Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getFFmpegVersion`

## Publishing

### Setup GitHub Packages

1. Create Personal Access Token:
   - Go to GitHub Settings → Developer settings → Personal access tokens
   - Generate token with `write:packages` scope

2. Configure credentials:

   ```bash
   # ~/.gradle/gradle.properties
   gpr.user=YOUR_GITHUB_USERNAME
   gpr.key=YOUR_GITHUB_TOKEN
   ```

3. Update repository URL in `build.gradle`:
   ```groovy
   url = uri("https://maven.pkg.github.com/YOUR_USERNAME/smart-ffmpeg-android")
   ```

### Publish

```bash
./gradlew publish
```

## Troubleshooting

### NDK Not Found

```bash
export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/26.x.xxxx
```

### FFmpeg Build Fails

- Check NDK version (must be r21+)
- Verify all build tools are installed
- Check disk space (needs ~2GB)

### AAR Too Large

- Review FFmpeg configure flags
- Disable unnecessary codecs
- Use `--enable-small` flag
- Strip debug symbols

### Native Library Not Found

- Verify `jniLibs` structure
- Check CMakeLists.txt paths
- Ensure all ABIs are built

## CI/CD Integration

### GitHub Actions Example

```yaml
name: Build AAR

on: [push]

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - name: Set up JDK 11
        uses: actions/setup-java@v3
        with:
          java-version: "11"

      - name: Setup Android SDK
        uses: android-actions/setup-android@v2

      - name: Build FFmpeg
        run: ./build_ffmpeg.sh

      - name: Build AAR
        run: ./gradlew assembleRelease

      - name: Upload AAR
        uses: actions/upload-artifact@v3
        with:
          name: smart-ffmpeg-android
          path: build/outputs/aar/*.aar
```

## Size Optimization Tips

1. **Minimal Codecs**: Only enable required decoders
2. **No Encoders**: Disable all encoders
3. **No Filters**: Disable filter subsystem
4. **No Network**: Disable network protocols
5. **Strip Symbols**: Use `--strip-all` in NDK
6. **Enable Small**: Use `--enable-small` flag
7. **Optimize**: Use `-Os` optimization level

## License Compliance

This build is LGPL-only:

- ✅ No GPL components
- ✅ No GPL codecs (x264, x265)
- ✅ No GPL filters
- ✅ Dynamic linking only
- ✅ Source code available

Verify with:

```bash
./ffmpeg -version | grep configuration
```

Should NOT contain any GPL flags.
