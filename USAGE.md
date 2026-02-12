# Smart FFmpeg Android - Usage Guide

## Installation

### Step 1: Add GitHub Packages repository

Add to your project's `settings.gradle` or `settings.gradle.kts`:

```kotlin
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven {
            url = uri("https://maven.pkg.github.com/Daronec/smart-ffmpeg-android")
            credentials {
                username = project.findProperty("gpr.user") as String? ?: System.getenv("GPR_USER")
                password = project.findProperty("gpr.key") as String? ?: System.getenv("GPR_KEY")
            }
        }
    }
}
```

### Step 2: Add credentials

Create or edit `~/.gradle/gradle.properties`:

```properties
gpr.user=YOUR_GITHUB_USERNAME
gpr.key=YOUR_GITHUB_PERSONAL_ACCESS_TOKEN
```

To create a Personal Access Token:

1. Go to GitHub Settings → Developer settings → Personal access tokens → Tokens (classic)
2. Generate new token with `read:packages` scope
3. Copy the token to `gpr.key`

### Step 3: Add dependency

Add to your app's `build.gradle` or `build.gradle.kts`:

```kotlin
dependencies {
    implementation("com.smartmedia:smart-ffmpeg-android:1.0.3")
}
```

## Usage

### Extract Video Thumbnail

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge

// Extract thumbnail at 5 seconds
val videoPath = "/path/to/video.mp4"
val timeMs = 5000L  // 5 seconds
val width = 640
val height = 360

val rgbaData = SmartFfmpegBridge.extractThumbnail(
    videoPath = videoPath,
    timeMs = timeMs,
    width = width,
    height = height
)

if (rgbaData != null) {
    // Convert RGBA to Bitmap
    val bitmap = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
    val buffer = ByteBuffer.wrap(rgbaData)
    bitmap.copyPixelsFromBuffer(buffer)

    // Use bitmap...
    imageView.setImageBitmap(bitmap)
}
```

### Get Video Duration

```kotlin
val videoPath = "/path/to/video.mp4"
val durationMs = SmartFfmpegBridge.getVideoDuration(videoPath)

if (durationMs > 0) {
    val durationSeconds = durationMs / 1000.0
    println("Video duration: $durationSeconds seconds")
}
```

### Get Video Metadata

```kotlin
val videoPath = "/path/to/video.mp4"
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)

metadata?.let {
    val width = it["width"] as? Int
    val height = it["height"] as? Int
    val duration = it["duration"] as? Long
    val codec = it["codec"] as? String

    println("Resolution: ${width}x${height}")
    println("Duration: $duration ms")
    println("Codec: $codec")
}
```

### Get FFmpeg Version

```kotlin
val version = SmartFfmpegBridge.getFFmpegVersion()
println("FFmpeg version: $version")
```

## Requirements

- **Minimum SDK**: 26 (Android 8.0)
- **Target SDK**: 34
- **Architecture**: arm64-v8a

## Features

- ✅ Extract video thumbnails at any timestamp
- ✅ Get video duration
- ✅ Get video metadata (resolution, codec, etc.)
- ✅ Hardware acceleration support
- ✅ RGBA output format
- ✅ Comprehensive test suite

## Example

See the [example app](example/) for a complete working example.

## License

[Your License Here]

## Support

For issues and questions, please visit:
https://github.com/Daronec/smart-ffmpeg-android/issues
