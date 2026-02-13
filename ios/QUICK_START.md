# Quick Start - iOS

Get started with Smart FFmpeg Bridge for iOS in 5 minutes.

## Installation

### Option 1: CocoaPods (Recommended)

1. Add to your `Podfile`:

```ruby
pod 'SmartFfmpegBridge', :git => 'https://github.com/Daronec/smart-ffmpeg-android.git', :tag => 'v1.0.4'
```

2. Install:

```bash
pod install
```

### Option 2: Manual Integration

1. Copy files to your project:
   - `ios/Classes/SmartFfmpegBridge.h`
   - `ios/Classes/SmartFfmpegBridge.m`
   - `ios/Classes/SmartFfmpegBridge.swift`

2. Add ffmpeg-kit dependency to your `Podfile`:

```ruby
pod 'ffmpeg-kit-ios-full', '~> 6.0'
```

3. Run `pod install`

## Basic Usage

### Swift

```swift
import SmartFfmpegBridge

// 1. Extract thumbnail
let thumbnail = SmartFfmpegBridgeSwift.extractThumbnailImage(
    fromVideo: "/path/to/video.mp4",
    atTime: 5000,  // 5 seconds
    width: 640,
    height: 360
)

if let image = thumbnail {
    imageView.image = image
}

// 2. Get video info
let duration = SmartFfmpegBridgeSwift.getVideoDuration("/path/to/video.mp4")
print("Duration: \(duration) ms")

// 3. Get metadata
if let metadata = SmartFfmpegBridgeSwift.getVideoMetadata("/path/to/video.mp4") {
    print("Width: \(metadata["width"] ?? 0)")
    print("Height: \(metadata["height"] ?? 0)")
    print("Codec: \(metadata["codec"] ?? "unknown")")
}
```

### Objective-C

```objc
#import <SmartFfmpegBridge/SmartFfmpegBridge.h>

// 1. Extract thumbnail (raw RGBA data)
NSData *rgbaData = [SmartFfmpegBridge extractThumbnailFromVideo:@"/path/to/video.mp4"
                                                         atTime:5000
                                                          width:640
                                                         height:360];

// 2. Get video info
int64_t duration = [SmartFfmpegBridge getVideoDuration:@"/path/to/video.mp4"];
NSLog(@"Duration: %lld ms", duration);

// 3. Get metadata
NSDictionary *metadata = [SmartFfmpegBridge getVideoMetadata:@"/path/to/video.mp4"];
NSLog(@"Width: %@", metadata[@"width"]);
NSLog(@"Height: %@", metadata[@"height"]);
```

## Common Use Cases

### 1. Video Thumbnail Gallery

```swift
func createThumbnails(for videoPath: String, count: Int) -> [UIImage] {
    let duration = SmartFfmpegBridgeSwift.getVideoDuration(videoPath)
    let interval = duration / Int64(count)

    var thumbnails: [UIImage] = []

    for i in 0..<count {
        let timeMs = Int64(i) * interval
        if let thumbnail = SmartFfmpegBridgeSwift.extractThumbnailImage(
            fromVideo: videoPath,
            atTime: timeMs,
            width: 320,
            height: 180
        ) {
            thumbnails.append(thumbnail)
        }
    }

    return thumbnails
}
```

### 2. Video Preview

```swift
func showVideoPreview(videoPath: String) {
    // Get first frame as preview
    if let preview = SmartFfmpegBridgeSwift.extractThumbnailImage(
        fromVideo: videoPath,
        atTime: 0,
        width: 1280,
        height: 720
    ) {
        previewImageView.image = preview
    }

    // Show duration
    let duration = SmartFfmpegBridgeSwift.getVideoDuration(videoPath)
    let seconds = Double(duration) / 1000.0
    durationLabel.text = String(format: "%.1f sec", seconds)
}
```

### 3. Video Validation

```swift
func validateVideo(path: String) -> Bool {
    guard let metadata = SmartFfmpegBridgeSwift.getVideoMetadata(path),
          let width = metadata["width"] as? Int,
          let height = metadata["height"] as? Int,
          let duration = metadata["duration"] as? Int64 else {
        return false
    }

    // Check minimum requirements
    let minWidth = 640
    let minHeight = 480
    let minDuration: Int64 = 1000  // 1 second

    return width >= minWidth &&
           height >= minHeight &&
           duration >= minDuration
}
```

## Permissions

Add to your `Info.plist`:

```xml
<key>NSPhotoLibraryUsageDescription</key>
<string>We need access to your photo library to process videos</string>
```

## Troubleshooting

### "Module not found"

- Make sure you ran `pod install`
- Open `.xcworkspace` file, not `.xcodeproj`

### "Failed to extract thumbnail"

- Check if video file exists
- Verify file path is correct (not a URL)
- Check video format is supported

### Large app size

- Use `ffmpeg-kit-ios-min` instead of `ffmpeg-kit-ios-full`
- Enable app thinning in Xcode

## Next Steps

- Read the [full documentation](README.md)
- Check out the [example project](Example/)
- Learn about [Flutter integration](FLUTTER_INTEGRATION.md)
- Compare [Android vs iOS](../PLATFORM_COMPARISON.md)

## Support

Issues? Visit: https://github.com/Daronec/smart-ffmpeg-android/issues
