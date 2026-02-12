# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2024-12-XX

### Added

#### Video Playback

- Full-featured FFmpeg video player with native C implementation
- Hardware acceleration support (MediaCodec)
- Audio/Video synchronization with multiple clock sources
- Frame-accurate seeking (exact and fast modes)
- Playback speed control (0.5x - 3.0x)
- Pause/Resume functionality
- Position tracking and duration reporting

#### Media Processing

- Thumbnail extraction from video files
- Video metadata extraction (width, height, duration)
- Support for multiple video formats (MP4, AVI, FLV, MKV, WebM, etc.)
- FFmpeg version information

#### API

- `SmartFFmpegPlayer` - Kotlin API for video playback
- `SmartFfmpegBridge` - Kotlin API for thumbnail/metadata extraction
- Event-based callbacks for player state changes
- Surface-based rendering with SurfaceView/TextureView support

#### Architecture Support

- arm64-v8a (64-bit ARM)
- Optimized native libraries (~7-8MB per architecture)

#### Documentation

- Comprehensive README with examples
- Integration guide for Android projects
- Security guidelines for token management
- Build instructions for FFmpeg
- Publishing guide for GitHub Packages

### Technical Details

#### FFmpeg Configuration

- FFmpeg 6.1 (LGPL 2.1)
- Optimized for size with `--enable-small`
- Hardware decoders: h264_mediacodec, hevc_mediacodec, mpeg4_mediacodec
- Software decoders: h264, hevc, mpeg4, vp8, vp9
- Demuxers: mov, mp4, matroska, avi, flv
- No GPL components included

#### Native Components

- 50+ C source files for player implementation
- OpenGL ES rendering
- OpenSL ES audio output
- JNI bridge for Kotlin integration
- Thread-safe operations
- Proper memory management

### Requirements

- Android API 21+ (Android 5.0 Lollipop)
- Java 11+
- Gradle 8.0+
- CMake 3.22.1+

### Known Limitations

- Only arm64-v8a architecture included in initial release
- No subtitle support in this release
- No streaming support (HTTP/RTSP) in this release

### License

- LGPL 2.1 (compatible with FFmpeg)
- Source code available on GitHub

---

## [Unreleased]

### Planned for 1.1.0

- [ ] armeabi-v7a (32-bit ARM) support
- [ ] Subtitle rendering
- [ ] HTTP/RTSP streaming support
- [ ] Picture-in-Picture mode
- [ ] Background playback
- [ ] Playlist support

### Planned for 1.2.0

- [ ] x86_64 support (for emulators)
- [ ] Hardware encoding
- [ ] Video filters
- [ ] Audio effects
- [ ] Multi-track audio support

---

## Release Notes

### How to Upgrade

From source:

```bash
git pull origin main
./gradlew clean assembleRelease
```

From GitHub Packages:

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg-android:1.0.0'
}
```

### Breaking Changes

None (initial release)

### Deprecations

None (initial release)

### Security

- No known security vulnerabilities
- Regular FFmpeg security updates will be applied
- See SECURITY.md for reporting vulnerabilities

---

[1.0.0]: https://github.com/Daronec/smart-ffmpeg-android/releases/tag/v1.0.0
[Unreleased]: https://github.com/Daronec/smart-ffmpeg-android/compare/v1.0.0...HEAD
