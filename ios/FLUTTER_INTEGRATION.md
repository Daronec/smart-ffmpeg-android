# Flutter Integration Guide - iOS

This guide explains how to integrate Smart FFmpeg Bridge iOS into a Flutter plugin.

## Project Structure

```
your_flutter_plugin/
├── android/                    # Android implementation (existing)
├── ios/                        # iOS implementation (new)
│   ├── Classes/
│   │   ├── YourPlugin.swift   # Flutter plugin bridge
│   │   └── SmartFfmpegBridge/ # Copy iOS bridge files here
│   └── your_plugin.podspec
├── lib/
│   └── your_plugin.dart       # Dart API
└── pubspec.yaml
```

## Step 1: Copy iOS Bridge Files

Copy the iOS bridge files to your Flutter plugin:

```bash
# From smart-ffmpeg-android repository
cp -r ios/Classes/* your_flutter_plugin/ios/Classes/SmartFfmpegBridge/
```

## Step 2: Update Podspec

Edit `ios/your_plugin.podspec`:

```ruby
Pod::Spec.new do |s|
  s.name             = 'your_plugin'
  s.version          = '0.0.1'
  s.summary          = 'Your plugin description'
  s.homepage         = 'https://example.com'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Your Company' => 'email@example.com' }
  s.source           = { :path => '.' }

  s.source_files = 'Classes/**/*'
  s.public_header_files = 'Classes/**/*.h'

  s.dependency 'Flutter'
  s.dependency 'ffmpeg-kit-ios-full', '~> 6.0'  # Add FFmpeg dependency

  s.platform = :ios, '12.0'
  s.swift_version = '5.0'

  s.pod_target_xcconfig = {
    'DEFINES_MODULE' => 'YES',
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386'
  }
end
```

## Step 3: Create Flutter Plugin Bridge

Create `ios/Classes/YourPlugin.swift`:

```swift
import Flutter
import UIKit

public class YourPlugin: NSObject, FlutterPlugin {

    public static func register(with registrar: FlutterPluginRegistrar) {
        let channel = FlutterMethodChannel(
            name: "your_plugin",
            binaryMessenger: registrar.messenger()
        )
        let instance = YourPlugin()
        registrar.addMethodCallDelegate(instance, channel: channel)
    }

    public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        switch call.method {
        case "extractThumbnail":
            handleExtractThumbnail(call, result: result)

        case "getVideoDuration":
            handleGetVideoDuration(call, result: result)

        case "getVideoMetadata":
            handleGetVideoMetadata(call, result: result)

        case "getFFmpegVersion":
            handleGetFFmpegVersion(result: result)

        default:
            result(FlutterMethodNotImplemented)
        }
    }

    // MARK: - Method Handlers

    private func handleExtractThumbnail(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any],
              let videoPath = args["videoPath"] as? String,
              let timeMs = args["timeMs"] as? Int64,
              let width = args["width"] as? Int,
              let height = args["height"] as? Int else {
            result(FlutterError(
                code: "INVALID_ARGUMENTS",
                message: "Missing required arguments",
                details: nil
            ))
            return
        }

        DispatchQueue.global(qos: .userInitiated).async {
            let rgbaData = SmartFfmpegBridgeSwift.extractThumbnailData(
                fromVideo: videoPath,
                atTime: timeMs,
                width: width,
                height: height
            )

            DispatchQueue.main.async {
                if let data = rgbaData {
                    result(FlutterStandardTypedData(bytes: data))
                } else {
                    result(FlutterError(
                        code: "EXTRACTION_FAILED",
                        message: "Failed to extract thumbnail",
                        details: nil
                    ))
                }
            }
        }
    }

    private func handleGetVideoDuration(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any],
              let videoPath = args["videoPath"] as? String else {
            result(FlutterError(
                code: "INVALID_ARGUMENTS",
                message: "Missing videoPath argument",
                details: nil
            ))
            return
        }

        let duration = SmartFfmpegBridgeSwift.getVideoDuration(videoPath)
        result(duration)
    }

    private func handleGetVideoMetadata(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
        guard let args = call.arguments as? [String: Any],
              let videoPath = args["videoPath"] as? String else {
            result(FlutterError(
                code: "INVALID_ARGUMENTS",
                message: "Missing videoPath argument",
                details: nil
            ))
            return
        }

        if let metadata = SmartFfmpegBridgeSwift.getVideoMetadata(videoPath) {
            result(metadata)
        } else {
            result(FlutterError(
                code: "METADATA_FAILED",
                message: "Failed to get video metadata",
                details: nil
            ))
        }
    }

    private func handleGetFFmpegVersion(result: @escaping FlutterResult) {
        let version = SmartFfmpegBridgeSwift.getFFmpegVersion()
        result(version)
    }
}
```

## Step 4: Dart API

Your Dart API should match both Android and iOS implementations:

```dart
import 'dart:typed_data';
import 'package:flutter/services.dart';

class YourPlugin {
  static const MethodChannel _channel = MethodChannel('your_plugin');

  /// Extract thumbnail from video
  static Future<Uint8List?> extractThumbnail({
    required String videoPath,
    required int timeMs,
    required int width,
    required int height,
  }) async {
    try {
      final result = await _channel.invokeMethod('extractThumbnail', {
        'videoPath': videoPath,
        'timeMs': timeMs,
        'width': width,
        'height': height,
      });
      return result as Uint8List?;
    } catch (e) {
      print('Error extracting thumbnail: $e');
      return null;
    }
  }

  /// Get video duration in milliseconds
  static Future<int> getVideoDuration(String videoPath) async {
    try {
      final result = await _channel.invokeMethod('getVideoDuration', {
        'videoPath': videoPath,
      });
      return result as int;
    } catch (e) {
      print('Error getting video duration: $e');
      return -1;
    }
  }

  /// Get video metadata
  static Future<Map<String, dynamic>?> getVideoMetadata(String videoPath) async {
    try {
      final result = await _channel.invokeMethod('getVideoMetadata', {
        'videoPath': videoPath,
      });
      return Map<String, dynamic>.from(result as Map);
    } catch (e) {
      print('Error getting video metadata: $e');
      return null;
    }
  }

  /// Get FFmpeg version
  static Future<String> getFFmpegVersion() async {
    try {
      final result = await _channel.invokeMethod('getFFmpegVersion');
      return result as String;
    } catch (e) {
      print('Error getting FFmpeg version: $e');
      return 'Unknown';
    }
  }
}
```

## Step 5: Install Dependencies

Run in your Flutter project:

```bash
cd ios
pod install
cd ..
```

## Step 6: Test

```dart
import 'package:your_plugin/your_plugin.dart';

void testPlugin() async {
  // Get FFmpeg version
  final version = await YourPlugin.getFFmpegVersion();
  print('FFmpeg version: $version');

  // Extract thumbnail
  final videoPath = '/path/to/video.mp4';
  final thumbnail = await YourPlugin.extractThumbnail(
    videoPath: videoPath,
    timeMs: 5000,
    width: 640,
    height: 360,
  );

  if (thumbnail != null) {
    print('Thumbnail extracted: ${thumbnail.length} bytes');
  }

  // Get metadata
  final metadata = await YourPlugin.getVideoMetadata(videoPath);
  print('Metadata: $metadata');
}
```

## Platform-Specific Notes

### iOS Permissions

Add to `ios/Runner/Info.plist`:

```xml
<key>NSPhotoLibraryUsageDescription</key>
<string>We need access to your photo library to process videos</string>
<key>NSCameraUsageDescription</key>
<string>We need access to your camera to record videos</string>
```

### File Paths

iOS uses different file paths than Android. Make sure to:

- Use `path_provider` package to get proper directories
- Handle file URLs correctly (convert `file://` URLs to paths)

### Background Processing

For long video processing, consider using background tasks:

```swift
import BackgroundTasks

// Register background task
BGTaskScheduler.shared.register(
    forTaskWithIdentifier: "com.example.videoProcessing",
    using: nil
) { task in
    // Process video in background
}
```

## Troubleshooting

### Pod Install Fails

If `pod install` fails with FFmpeg dependency:

```bash
cd ios
pod cache clean --all
pod deintegrate
pod install --repo-update
```

### Build Errors

1. Clean build folder: Product → Clean Build Folder (Cmd+Shift+K)
2. Delete derived data: `rm -rf ~/Library/Developer/Xcode/DerivedData`
3. Reinstall pods: `cd ios && pod install`

### Large Binary Size

FFmpeg adds ~50MB to your app. To reduce size:

- Use `ffmpeg-kit-ios-min` instead of `ffmpeg-kit-ios-full`
- Enable bitcode and app thinning in Xcode

## Example Projects

See:

- [iOS Example](Example/) - Native iOS example
- [Flutter Example](../../example/) - Flutter plugin example

## Support

For issues specific to iOS implementation:
https://github.com/Daronec/smart-ffmpeg-android/issues
