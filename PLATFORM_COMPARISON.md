# Platform Comparison: Android vs iOS

This document compares the Android and iOS implementations of Smart FFmpeg Bridge.

## Architecture Overview

### Android

- **Language**: Kotlin + C (JNI)
- **FFmpeg Integration**: Custom compiled FFmpeg libraries
- **Native Bridge**: JNI (Java Native Interface)
- **Build System**: Gradle + CMake
- **Distribution**: Maven (GitHub Packages)

### iOS

- **Language**: Swift + Objective-C
- **FFmpeg Integration**: ffmpeg-kit CocoaPod
- **Native Bridge**: Objective-C runtime
- **Build System**: CocoaPods + Xcode
- **Distribution**: CocoaPods (Git)

## API Comparison

Both platforms provide identical functionality with platform-specific implementations:

| Feature            | Android | iOS | Notes                   |
| ------------------ | ------- | --- | ----------------------- |
| Extract Thumbnail  | ✅      | ✅  | Returns RGBA byte array |
| Get Duration       | ✅      | ✅  | Returns milliseconds    |
| Get Metadata       | ✅      | ✅  | Returns Map/Dictionary  |
| Get FFmpeg Version | ✅      | ✅  | Returns version string  |

## Method Signatures

### Android (Kotlin)

```kotlin
object SmartFfmpegBridge {
    fun extractThumbnail(
        videoPath: String,
        timeMs: Long,
        width: Int,
        height: Int
    ): ByteArray?

    fun getVideoDuration(videoPath: String): Long

    fun getVideoMetadata(videoPath: String): Map<String, Any>?

    fun getFFmpegVersion(): String
}
```

### iOS (Swift)

```swift
class SmartFfmpegBridgeSwift {
    static func extractThumbnailData(
        fromVideo videoPath: String,
        atTime timeMs: Int64,
        width: Int,
        height: Int
    ) -> Data?

    static func getVideoDuration(_ videoPath: String) -> Int64

    static func getVideoMetadata(_ videoPath: String) -> [String: Any]?

    static func getFFmpegVersion() -> String
}
```

## Implementation Differences

### Thumbnail Extraction

**Android (C/JNI)**:

- Direct FFmpeg API calls
- Manual memory management
- Hardware buffer support (AHardwareBuffer)
- Custom scaling with swscale

**iOS (Objective-C + ffmpeg-kit)**:

- FFmpeg command-line interface via ffmpeg-kit
- Automatic memory management
- Temporary file for raw output
- Built-in scaling

### Performance

| Aspect          | Android           | iOS                      |
| --------------- | ----------------- | ------------------------ |
| Thumbnail Speed | Fast (direct API) | Medium (CLI + file I/O)  |
| Memory Usage    | Low (streaming)   | Medium (temp files)      |
| Startup Time    | Fast              | Medium (ffmpeg-kit init) |
| Binary Size     | ~50MB             | ~50MB                    |

### File Handling

**Android**:

- Direct file path access
- Content URIs supported
- Storage Access Framework

**iOS**:

- File path or URL
- Sandbox restrictions
- Photo Library integration

## Platform-Specific Features

### Android Only

- Hardware acceleration (MediaCodec)
- AHardwareBuffer for zero-copy
- OpenSL ES audio
- Android-specific video formats

### iOS Only

- UIImage conversion helper
- AVFoundation integration
- Metal rendering support
- iOS-specific video formats

## Dependencies

### Android

```gradle
// Native FFmpeg libraries (included)
- libavcodec.so
- libavformat.so
- libavutil.so
- libswscale.so
- libswresample.so
- libavfilter.so
- libavdevice.so

// Android dependencies
- androidx.core:core-ktx
```

### iOS

```ruby
# CocoaPods
pod 'ffmpeg-kit-ios-full', '~> 6.0'

# System frameworks
- Foundation
- AVFoundation
- CoreMedia
- CoreVideo
```

## Minimum Requirements

| Requirement     | Android              | iOS           |
| --------------- | -------------------- | ------------- |
| Minimum Version | Android 8.0 (API 26) | iOS 12.0      |
| Architecture    | arm64-v8a            | arm64, x86_64 |
| Storage         | ~50MB                | ~50MB         |
| RAM             | 512MB+               | 512MB+        |

## Testing

### Android

- JUnit unit tests
- Android instrumented tests
- Gradle test runner

### iOS

- XCTest unit tests
- UI tests
- Xcode test runner

## Distribution

### Android

```gradle
// GitHub Packages
implementation("com.smartmedia:smart-ffmpeg-android:1.0.4")
```

### iOS

```ruby
# CocoaPods
pod 'SmartFfmpegBridge', :git => 'https://github.com/Daronec/smart-ffmpeg-android.git', :tag => 'v1.0.4'
```

## Flutter Integration

Both platforms integrate seamlessly with Flutter using platform channels:

```dart
// Same Dart API for both platforms
final thumbnail = await YourPlugin.extractThumbnail(
  videoPath: videoPath,
  timeMs: 5000,
  width: 640,
  height: 360,
);
```

Platform-specific code is handled automatically by Flutter's platform channels.

## Future Improvements

### Android

- [ ] Add more hardware acceleration options
- [ ] Support more architectures (x86, armeabi-v7a)
- [ ] Optimize memory usage
- [ ] Add video encoding support

### iOS

- [ ] Optimize thumbnail extraction (direct API instead of CLI)
- [ ] Add hardware acceleration (VideoToolbox)
- [ ] Support macOS
- [ ] Add video encoding support

## Migration Guide

If you're migrating from Android-only to cross-platform:

1. **Keep Android code unchanged** - No modifications needed
2. **Add iOS implementation** - Copy iOS files to your project
3. **Update Flutter plugin** - Add iOS platform channel handlers
4. **Test both platforms** - Ensure identical behavior

## Conclusion

Both implementations provide the same functionality with platform-appropriate approaches:

- **Android**: Direct FFmpeg API for maximum performance
- **iOS**: ffmpeg-kit for ease of integration and maintenance

Choose the implementation that best fits your platform needs, or use both for cross-platform Flutter apps.
