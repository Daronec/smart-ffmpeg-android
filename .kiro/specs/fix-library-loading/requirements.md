# Requirements Document: Fix Library Loading

## Introduction

This bugfix addresses a critical runtime error in the smart-ffmpeg-android library where the native library fails to load due to a name mismatch. The Kotlin code attempts to load `libffmpeg_bridge.so`, but the actual compiled library is named `libsmart_ffmpeg.so`, causing an immediate crash when the library is initialized.

## Glossary

- **Native_Library**: The compiled shared object file (.so) containing FFmpeg native code
- **System_Loader**: Android's System.loadLibrary() mechanism for loading native libraries
- **Library_Name**: The identifier used in System.loadLibrary() (without "lib" prefix or ".so" extension)
- **CMake_Target**: The library name defined in CMakeLists.txt using add_library()
- **SmartFfmpegBridge**: The Kotlin class that provides the JNI bridge to native FFmpeg functions

## Requirements

### Requirement 1: Correct Library Name Loading

**User Story:** As a developer integrating smart-ffmpeg-android, I want the library to load successfully at runtime, so that my application doesn't crash on initialization.

#### Acceptance Criteria

1. WHEN the SmartFfmpegBridge class is initialized, THE System_Loader SHALL load the library named "smart_ffmpeg"
2. WHEN the library is loaded, THE System_Loader SHALL successfully find and load libsmart_ffmpeg.so from the AAR
3. THE Library_Name in System.loadLibrary() SHALL match the CMake_Target name defined in CMakeLists.txt
4. WHEN the library loads successfully, THE SmartFfmpegBridge SHALL be ready to execute native methods without errors

### Requirement 2: Version Update

**User Story:** As a library maintainer, I want to increment the version number after fixing the bug, so that users can identify the fixed version.

#### Acceptance Criteria

1. WHEN the library loading fix is applied, THE version number SHALL be updated to 1.0.1
2. THE version update SHALL be reflected in gradle.properties
3. THE version update SHALL follow semantic versioning conventions (patch increment for bugfix)

### Requirement 3: Verification

**User Story:** As a developer, I want to verify that the library loads correctly, so that I can confirm the fix works.

#### Acceptance Criteria

1. WHEN the fixed library is built, THE AAR SHALL contain libsmart_ffmpeg.so
2. WHEN the SmartFfmpegBridge class is loaded in a test application, THE initialization SHALL complete without throwing UnsatisfiedLinkError
3. WHEN getFFmpegVersion() is called after successful loading, THE method SHALL return a valid version string
