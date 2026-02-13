# Project Structure

This document describes the complete project structure for both Android and iOS implementations.

## Overview

```
smart-ffmpeg-bridge/
├── android/                          # Android implementation
│   ├── src/
│   │   ├── main/
│   │   │   ├── cpp/                  # Native C/C++ code
│   │   │   │   ├── ffmpeg_bridge_jni.c
│   │   │   │   ├── CMakeLists.txt
│   │   │   │   └── native_media_engine/
│   │   │   │       ├── ffmpeg_player/
│   │   │   │       ├── include/      # FFmpeg headers
│   │   │   │       └── jniLibs/      # FFmpeg .so files
│   │   │   └── kotlin/
│   │   │       └── com/smartmedia/ffmpeg/
│   │   │           ├── SmartFFmpegPlayer.kt
│   │   │           └── SmartFfmpegBridge.kt
│   │   ├── test/                     # Unit tests
│   │   └── androidTest/              # Instrumented tests
│   ├── build.gradle                  # Android build config
│   └── gradle.properties
│
├── ios/                              # iOS implementation
│   ├── Classes/                      # iOS source files
│   │   ├── SmartFfmpegBridge.h      # Objective-C header
│   │   ├── SmartFfmpegBridge.m      # Objective-C implementation
│   │   └── SmartFfmpegBridge.swift  # Swift wrapper
│   ├── Example/                      # iOS example app
│   │   └── ViewController.swift
│   ├── SmartFfmpegBridge.podspec    # CocoaPods spec
│   ├── README.md                     # iOS documentation
│   ├── QUICK_START.md               # iOS quick start
│   └── FLUTTER_INTEGRATION.md       # iOS Flutter guide
│
├── docs/                             # Documentation
│   ├── README.md                     # Main documentation
│   ├── PLATFORM_COMPARISON.md       # Android vs iOS
│   ├── FLUTTER_INTEGRATION.md       # Android Flutter guide
│   ├── USAGE.md                      # Android usage guide
│   └── INTEGRATION_GUIDE.md         # Android integration
│
├── .github/                          # GitHub configuration
│   └── workflows/                    # CI/CD workflows
│
├── LICENSE                           # LGPL 2.1 license
└── README.md                         # Project overview
```

## Android Structure

### Native Code (`src/main/cpp/`)

```
cpp/
├── ffmpeg_bridge_jni.c              # JNI bridge implementation
├── CMakeLists.txt                   # CMake build configuration
└── native_media_engine/
    ├── ffmpeg_player/               # FFmpeg player implementation
    │   ├── ffmpeg_player.c          # Main player
    │   ├── audio_renderer.c         # Audio rendering
    │   ├── video_renderer.c         # Video rendering
    │   ├── video_render_gl.c        # OpenGL rendering
    │   ├── video_render_hardware.c  # Hardware rendering
    │   ├── video_backend_mediacodec.c # MediaCodec backend
    │   ├── avsync.c                 # A/V synchronization
    │   ├── packet_queue.c           # Packet queue
    │   ├── frame_queue.c            # Frame queue
    │   └── clock.c                  # Clock management
    ├── include/                     # FFmpeg headers
    │   ├── libavcodec/
    │   ├── libavformat/
    │   ├── libavutil/
    │   ├── libswscale/
    │   └── libswresample/
    └── jniLibs/arm64-v8a/          # FFmpeg libraries
        ├── libavcodec.so
        ├── libavformat.so
        ├── libavutil.so
        ├── libswscale.so
        ├── libswresample.so
        ├── libavfilter.so
        └── libavdevice.so
```

### Kotlin Code (`src/main/kotlin/`)

```
kotlin/com/smartmedia/ffmpeg/
├── SmartFFmpegPlayer.kt             # Video player API
└── SmartFfmpegBridge.kt             # Utility functions API
```

### Tests

```
src/
├── test/                            # JVM unit tests
│   └── kotlin/com/smartmedia/ffmpeg/
│       ├── LibraryNameConsistencyTest.kt
│       ├── VersionValidationTest.kt
│       ├── AarContentVerificationTest.kt
│       ├── ThumbnailBytesValidationTest.kt
│       └── RGBAByteArrayTest.kt
└── androidTest/                     # Android instrumented tests
    └── kotlin/com/smartmedia/ffmpeg/
        ├── LibraryLoadingIntegrationTest.kt
        ├── NativeMethodInvocationTest.kt
        └── ThumbnailGenerationTest.kt
```

## iOS Structure

### Objective-C Implementation

```
ios/Classes/
├── SmartFfmpegBridge.h              # Public header
└── SmartFfmpegBridge.m              # Implementation
    ├── extractThumbnailFromVideo    # Thumbnail extraction
    ├── getVideoDuration             # Duration getter
    ├── getVideoMetadata             # Metadata getter
    └── getFFmpegVersion             # Version getter
```

### Swift Wrapper

```
ios/Classes/
└── SmartFfmpegBridge.swift          # Swift convenience API
    ├── SmartFfmpegBridgeSwift       # Main class
    │   ├── extractThumbnailImage    # Returns UIImage
    │   ├── extractThumbnailData     # Returns Data
    │   ├── getVideoDuration
    │   ├── getVideoMetadata
    │   └── getFFmpegVersion
    └── VideoMetadata                # Type-safe metadata
```

### Example App

```
ios/Example/
└── ViewController.swift             # Example usage
    ├── selectVideoButtonTapped      # Video picker
    ├── processVideo                 # Video processing
    ├── extractThumbnail             # Thumbnail extraction
    └── Helper methods
```

## Documentation Structure

```
docs/
├── README.md                        # Main documentation
├── PLATFORM_COMPARISON.md          # Platform differences
├── USAGE.md                         # Android usage guide
├── INTEGRATION_GUIDE.md            # Android integration
├── FLUTTER_INTEGRATION.md          # Android Flutter guide
├── STRUCTURE.md                     # Project structure
├── PUBLISH.md                       # Publishing guide
├── SECURITY.md                      # Security policy
└── CHANGELOG.md                     # Version history

ios/
├── README.md                        # iOS documentation
├── QUICK_START.md                  # iOS quick start
└── FLUTTER_INTEGRATION.md          # iOS Flutter guide
```

## Build Artifacts

### Android

```
build/
├── outputs/
│   └── aar/
│       └── smart-ffmpeg-android-release.aar  # ~50 MB
└── intermediates/
```

### iOS

```
Pods/
└── SmartFfmpegBridge/              # Installed via CocoaPods
    ├── Classes/
    └── Frameworks/
        └── ffmpegkit.framework     # ~50 MB
```

## Configuration Files

### Android

```
android/
├── build.gradle                     # Build configuration
├── gradle.properties               # Gradle properties
├── settings.gradle                 # Project settings
├── proguard-rules.pro             # ProGuard rules
└── CMakeLists.txt                  # CMake configuration
```

### iOS

```
ios/
├── SmartFfmpegBridge.podspec       # CocoaPods specification
└── Podfile                          # Dependencies (in example)
```

## Version Control

```
.git/
.gitignore                           # Git ignore rules
.github/
└── workflows/                       # CI/CD pipelines
    ├── android-build.yml
    └── ios-build.yml
```

## Key Files

| File                        | Purpose              | Platform |
| --------------------------- | -------------------- | -------- |
| `build.gradle`              | Android build config | Android  |
| `CMakeLists.txt`            | Native build config  | Android  |
| `ffmpeg_bridge_jni.c`       | JNI implementation   | Android  |
| `SmartFfmpegBridge.kt`      | Kotlin API           | Android  |
| `SmartFfmpegBridge.podspec` | CocoaPods spec       | iOS      |
| `SmartFfmpegBridge.h/.m`    | Objective-C API      | iOS      |
| `SmartFfmpegBridge.swift`   | Swift API            | iOS      |
| `README.md`                 | Main documentation   | Both     |
| `PLATFORM_COMPARISON.md`    | Platform differences | Both     |

## Dependencies

### Android Dependencies

- **FFmpeg**: Custom compiled (included in AAR)
- **AndroidX**: Core KTX
- **Testing**: JUnit, AndroidX Test

### iOS Dependencies

- **ffmpeg-kit**: ~6.0 (via CocoaPods)
- **System Frameworks**: Foundation, AVFoundation, CoreMedia, CoreVideo

## Size Breakdown

### Android AAR (~50 MB)

- FFmpeg libraries (.so): ~45 MB
- Native code: ~2 MB
- Kotlin code: ~100 KB
- Resources: ~1 MB

### iOS Framework (~50 MB)

- ffmpeg-kit: ~45 MB
- Native code: ~2 MB
- Swift/ObjC code: ~100 KB
- Resources: ~1 MB

## Development Workflow

1. **Android Development**:
   - Edit Kotlin/C code
   - Build with Gradle
   - Test with JUnit/Instrumented tests
   - Publish to GitHub Packages

2. **iOS Development**:
   - Edit Swift/Objective-C code
   - Build with Xcode
   - Test with XCTest
   - Publish via CocoaPods

3. **Cross-Platform**:
   - Maintain API parity
   - Update documentation
   - Test on both platforms
   - Version synchronization

## Next Steps

- [Android Integration](INTEGRATION_GUIDE.md)
- [iOS Quick Start](ios/QUICK_START.md)
- [Platform Comparison](PLATFORM_COMPARISON.md)
- [Flutter Integration](FLUTTER_INTEGRATION.md)
