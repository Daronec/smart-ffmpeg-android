# Implementation Plan: Fix Library Loading

## Overview

This bugfix requires changing one line of code to correct the native library name, updating the version number, and adding tests to verify the fix and prevent regression.

## Tasks

- [x] 1. Fix the library loading name in SmartFfmpegBridge.kt
  - Open `src/main/kotlin/com/smartmedia/ffmpeg/SmartFfmpegBridge.kt`
  - In the companion object's init block (line 13), change `System.loadLibrary("ffmpeg_bridge")` to `System.loadLibrary("smart_ffmpeg")`
  - _Requirements: 1.1, 1.3_

- [x] 2. Update version number to 1.0.1
  - Open `gradle.properties`
  - Update the version property from `1.0.0` to `1.0.1`
  - _Requirements: 2.1, 2.2, 2.3_

- [x]\* 3. Add library name consistency test
  - Create a test that verifies the library name in SmartFfmpegBridge.kt matches the CMake target in CMakeLists.txt
  - Parse both files and assert the names match ("smart_ffmpeg")
  - _Requirements: 1.1, 1.3_

- [x]\* 4. Add version validation test
  - Create a test that reads gradle.properties
  - Assert version is "1.0.1"
  - Assert version follows semantic versioning pattern (X.Y.Z)
  - _Requirements: 2.1, 2.2, 2.3_

- [x]\* 5. Add library loading integration test
  - Create an Android instrumented test
  - Attempt to load SmartFfmpegBridge class
  - Assert no UnsatisfiedLinkError is thrown
  - _Requirements: 1.2, 3.2_

- [x]\* 6. Add native method invocation test
  - Create an Android instrumented test
  - Load SmartFfmpegBridge and call getFFmpegVersion()
  - Assert result is non-null, non-empty, and matches FFmpeg version pattern
  - _Requirements: 1.4, 3.3_

- [x]\* 7. Add AAR content verification test
  - Create a test that extracts the built AAR
  - Verify libsmart_ffmpeg.so exists in architecture-specific folders (arm64-v8a, armeabi-v7a, x86, x86_64)
  - Assert files are not empty
  - _Requirements: 3.1_

- [x] 8. Checkpoint - Build and verify the fix
  - Run `./gradlew clean assembleRelease`
  - Manually verify the AAR contains libsmart_ffmpeg.so
  - If tests were implemented, ensure all tests pass
  - Ask the user if questions arise

## Notes

- Tasks marked with `*` are optional and can be skipped for faster deployment
- The core fix is extremely simple (one line change + version bump)
- Tests are recommended to prevent regression but not strictly required for the fix to work
- Manual verification in step 8 is critical to confirm the fix works
- This is a patch-level version bump (1.0.0 → 1.0.1) following semantic versioning
