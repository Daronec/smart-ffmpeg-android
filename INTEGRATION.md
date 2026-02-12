# Integrating Smart FFmpeg into Flutter Plugin

## Overview

This guide shows how to integrate the `smart-ffmpeg-android` library into your Flutter plugin.

## Step 1: Publish to GitHub Packages

```bash
cd smart-ffmpeg-android
./gradlew publish
```

## Step 2: Update Flutter Plugin

### android/build.gradle

```groovy
allprojects {
    repositories {
        google()
        mavenCentral()

        // Add GitHub Packages repository
        maven {
            name = "GitHubPackages"
            url = uri("https://maven.pkg.github.com/YOUR_USERNAME/smart-ffmpeg-android")
            credentials {
                username = project.findProperty("gpr.user") ?: System.getenv("GPR_USER")
                password = project.findProperty("gpr.key") ?: System.getenv("GPR_KEY")
            }
        }
    }
}
```

### android/build.gradle (module level)

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg:1.0.0'
}
```

## Step 3: Update Plugin Code

### SmartVideoThumbnailPlugin.kt

```kotlin
package ru.pathcreator.smart.video.thumbnails.smart_video_thumbnail

import com.smartmedia.ffmpeg.SmartFfmpegBridge
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel

class SmartVideoThumbnailPlugin: FlutterPlugin, MethodChannel.MethodCallHandler {
    private lateinit var channel: MethodChannel

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel = MethodChannel(binding.binaryMessenger, "smart_video_thumbnail")
        channel.setMethodCallHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "getThumbnail" -> {
                val videoPath = call.argument<String>("videoPath")
                val timeMs = call.argument<Long>("timeMs") ?: 0L
                val width = call.argument<Int>("width") ?: 0
                val height = call.argument<Int>("height") ?: 0

                if (videoPath == null) {
                    result.error("INVALID_ARGUMENT", "videoPath is required", null)
                    return
                }

                try {
                    val thumbnail = SmartFfmpegBridge.extractThumbnail(
                        videoPath, timeMs, width, height
                    )
                    result.success(thumbnail)
                } catch (e: Exception) {
                    result.error("EXTRACTION_ERROR", e.message, null)
                }
            }

            "getVideoDuration" -> {
                val videoPath = call.argument<String>("videoPath")
                if (videoPath == null) {
                    result.error("INVALID_ARGUMENT", "videoPath is required", null)
                    return
                }

                try {
                    val duration = SmartFfmpegBridge.getVideoDuration(videoPath)
                    result.success(duration)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }

            "getVideoMetadata" -> {
                val videoPath = call.argument<String>("videoPath")
                if (videoPath == null) {
                    result.error("INVALID_ARGUMENT", "videoPath is required", null)
                    return
                }

                try {
                    val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)
                    result.success(metadata)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }

            else -> result.notImplemented()
        }
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
    }
}
```

## Step 4: Remove Old FFmpeg Integration

1. Delete old FFmpeg .so files from `android/src/main/jniLibs/`
2. Delete old FFmpeg headers from `android/src/main/cpp/include/`
3. Delete old CMakeLists.txt or update to remove FFmpeg references
4. Update `.gitignore` to exclude FFmpeg binaries

## Step 5: Test Integration

```bash
cd your-flutter-plugin/example
flutter clean
flutter pub get
flutter run
```

## Benefits of This Approach

✅ **Separation of Concerns**: FFmpeg is a separate library
✅ **Easier Updates**: Update FFmpeg independently
✅ **Smaller Repository**: No large binaries in Flutter plugin repo
✅ **Reusability**: Use same FFmpeg library in multiple projects
✅ **CI/CD Friendly**: Automated builds and publishing
✅ **Version Control**: Semantic versioning for FFmpeg library

## Versioning Strategy

### FFmpeg Library Versions

- `1.0.x` - Bug fixes, no API changes
- `1.x.0` - New features, backward compatible
- `x.0.0` - Breaking changes

### Flutter Plugin Versions

Update plugin version when:

- FFmpeg library version changes
- Plugin API changes
- Bug fixes

## Troubleshooting

### Library Not Found

```
Could not resolve com.smartmedia:smart-ffmpeg:1.0.0
```

**Solution**: Check GitHub Packages credentials in `gradle.properties`

### Native Library Not Loaded

```
java.lang.UnsatisfiedLinkError: No implementation found for...
```

**Solution**:

1. Check AAR contains .so files
2. Verify ABI filters match
3. Clean and rebuild

### Symbol Not Found

```
java.lang.UnsatisfiedLinkError: ... symbol not found
```

**Solution**: Rebuild FFmpeg library with correct symbols

## CI/CD Setup

### GitHub Actions for Flutter Plugin

```yaml
name: Test Plugin

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3

      - uses: actions/setup-java@v3
        with:
          java-version: "11"

      - uses: subosito/flutter-action@v2
        with:
          flutter-version: "3.10.0"

      - name: Configure GitHub Packages
        run: |
          echo "gpr.user=${{ secrets.GPR_USER }}" >> gradle.properties
          echo "gpr.key=${{ secrets.GPR_KEY }}" >> gradle.properties

      - name: Get dependencies
        run: flutter pub get

      - name: Run tests
        run: flutter test

      - name: Build example
        run: |
          cd example
          flutter build apk
```

## Local Development

For local development without publishing:

```groovy
// android/build.gradle
dependencies {
    implementation project(':smart-ffmpeg-android')
}

// settings.gradle
include ':smart-ffmpeg-android'
project(':smart-ffmpeg-android').projectDir = new File('../smart-ffmpeg-android')
```

## Migration Checklist

- [ ] Build and publish smart-ffmpeg-android library
- [ ] Update Flutter plugin build.gradle
- [ ] Update plugin Kotlin code
- [ ] Remove old FFmpeg files
- [ ] Test on real device
- [ ] Update documentation
- [ ] Update CI/CD pipelines
- [ ] Tag new version
