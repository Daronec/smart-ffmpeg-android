# Smart FFmpeg Bridge - iOS

iOS implementation of Smart FFmpeg Bridge for video processing using FFmpeg.

## Features

- ✅ Extract video thumbnails at any timestamp
- ✅ Get video duration
- ✅ Get video metadata (resolution, codec, bitrate, etc.)
- ✅ RGBA output format
- ✅ Swift and Objective-C support
- ✅ iOS 12.0+

## Installation

### CocoaPods

Add to your `Podfile`:

```ruby
pod 'SmartFfmpegBridge', :git => 'https://github.com/Daronec/smart-ffmpeg-android.git', :tag => 'v1.0.4'
```

Then run:

```bash
pod install
```

### Swift Package Manager

Add to your `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/Daronec/smart-ffmpeg-android.git", from: "1.0.4")
]
```

## Usage

### Swift

```swift
import SmartFfmpegBridge

// Extract thumbnail as UIImage
let videoPath = "/path/to/video.mp4"
let timeMs: Int64 = 5000  // 5 seconds
let width = 640
let height = 360

if let thumbnail = SmartFfmpegBridgeSwift.extractThumbnailImage(
    fromVideo: videoPath,
    atTime: timeMs,
    width: width,
    height: height
) {
    imageView.image = thumbnail
}

// Get video duration
let durationMs = SmartFfmpegBridgeSwift.getVideoDuration(videoPath)
if durationMs > 0 {
    let durationSeconds = Double(durationMs) / 1000.0
    print("Video duration: \(durationSeconds) seconds")
}

// Get video metadata
if let metadata = SmartFfmpegBridgeSwift.getVideoMetadata(videoPath),
   let videoMetadata = VideoMetadata.from(dictionary: metadata) {
    print("Resolution: \(videoMetadata.width)x\(videoMetadata.height)")
    print("Duration: \(videoMetadata.duration) ms")
    print("Codec: \(videoMetadata.codec ?? "unknown")")
    print("Bitrate: \(videoMetadata.bitrate)")
}

// Get FFmpeg version
let version = SmartFfmpegBridgeSwift.getFFmpegVersion()
print("FFmpeg version: \(version)")
```

### Objective-C

```objc
#import <SmartFfmpegBridge/SmartFfmpegBridge.h>

// Extract thumbnail as raw RGBA data
NSString *videoPath = @"/path/to/video.mp4";
int64_t timeMs = 5000;  // 5 seconds
int width = 640;
int height = 360;

NSData *rgbaData = [SmartFfmpegBridge extractThumbnailFromVideo:videoPath
                                                         atTime:timeMs
                                                          width:width
                                                         height:height];

if (rgbaData) {
    // Convert RGBA data to UIImage
    // ... (see implementation in SmartFfmpegBridge.swift)
}

// Get video duration
int64_t durationMs = [SmartFfmpegBridge getVideoDuration:videoPath];
if (durationMs > 0) {
    double durationSeconds = durationMs / 1000.0;
    NSLog(@"Video duration: %.2f seconds", durationSeconds);
}

// Get video metadata
NSDictionary *metadata = [SmartFfmpegBridge getVideoMetadata:videoPath];
if (metadata) {
    NSNumber *width = metadata[@"width"];
    NSNumber *height = metadata[@"height"];
    NSNumber *duration = metadata[@"duration"];
    NSString *codec = metadata[@"codec"];

    NSLog(@"Resolution: %@x%@", width, height);
    NSLog(@"Duration: %@ ms", duration);
    NSLog(@"Codec: %@", codec);
}

// Get FFmpeg version
NSString *version = [SmartFfmpegBridge getFFmpegVersion];
NSLog(@"FFmpeg version: %@", version);
```

## Requirements

- iOS 12.0 or later
- Xcode 12.0 or later
- Swift 5.0 or later (for Swift usage)

## Architecture Support

- arm64 (iPhone 5s and later)
- x86_64 (Simulator)

## Dependencies

This library uses [ffmpeg-kit](https://github.com/arthenica/ffmpeg-kit) for FFmpeg functionality.

## API Reference

### SmartFfmpegBridge (Objective-C)

#### Methods

- `+ (NSData *)extractThumbnailFromVideo:atTime:width:height:` - Extract thumbnail as raw RGBA data
- `+ (int64_t)getVideoDuration:` - Get video duration in milliseconds
- `+ (NSDictionary *)getVideoMetadata:` - Get video metadata dictionary
- `+ (NSString *)getFFmpegVersion` - Get FFmpeg version string

### SmartFfmpegBridgeSwift (Swift)

#### Methods

- `extractThumbnailImage(fromVideo:atTime:width:height:)` - Extract thumbnail as UIImage
- `extractThumbnailData(fromVideo:atTime:width:height:)` - Extract thumbnail as raw RGBA data
- `getVideoDuration(_:)` - Get video duration in milliseconds
- `getVideoMetadata(_:)` - Get video metadata dictionary
- `getFFmpegVersion()` - Get FFmpeg version string

### VideoMetadata (Swift)

Type-safe wrapper for video metadata.

#### Properties

- `width: Int` - Video width in pixels
- `height: Int` - Video height in pixels
- `duration: Int64` - Video duration in milliseconds
- `codec: String?` - Video codec name
- `bitrate: Int64` - Video bitrate
- `format: String?` - Container format
- `frameRate: String?` - Frame rate

## Example Project

See the [example](example/) directory for a complete iOS example app.

## Platform Compatibility

This is the iOS implementation of Smart FFmpeg Bridge. For Android, see the main [README](../README.md).

## License

[Your License Here]

## Support

For issues and questions, please visit:
https://github.com/Daronec/smart-ffmpeg-android/issues
