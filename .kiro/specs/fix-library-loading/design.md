# Design Document: Fix Library Loading

## Overview

This bugfix resolves a critical library loading failure by correcting the native library name mismatch between the Kotlin code and the compiled CMake output. The fix is a simple one-line change that aligns the System.loadLibrary() call with the actual library name produced by the build system.

**Root Cause:** The SmartFfmpegBridge.kt file calls `System.loadLibrary("ffmpeg_bridge")`, but CMakeLists.txt defines `add_library(smart_ffmpeg SHARED ...)`, which produces `libsmart_ffmpeg.so`.

**Solution:** Change the library name parameter from "ffmpeg_bridge" to "smart_ffmpeg" to match the CMake target name.

## Architecture

The architecture remains unchanged. This is a configuration fix that ensures the JNI bridge can properly locate and load the native library at runtime.

### Current (Broken) Flow

```
SmartFfmpegBridge initialization
  → System.loadLibrary("ffmpeg_bridge")
  → Android searches for libffmpeg_bridge.so
  → Library not found in AAR
  → UnsatisfiedLinkError thrown
  → Application crashes
```

### Fixed Flow

```
SmartFfmpegBridge initialization
  → System.loadLibrary("smart_ffmpeg")
  → Android searches for libsmart_ffmpeg.so
  → Library found in AAR
  → Library loaded successfully
  → Native methods available
```

## Components and Interfaces

### Modified Component: SmartFfmpegBridge

**File:** `src/main/kotlin/com/smartmedia/ffmpeg/SmartFfmpegBridge.kt`

**Change Location:** Companion object's init block

**Before:**

```kotlin
companion object {
    init {
        System.loadLibrary("ffmpeg_bridge")  // ❌ Wrong name
    }
    // ... native methods
}
```

**After:**

```kotlin
companion object {
    init {
        System.loadLibrary("smart_ffmpeg")  // ✅ Correct name
    }
    // ... native methods
}
```

### Unchanged Components

- **CMakeLists.txt**: Already correctly defines `add_library(smart_ffmpeg SHARED ...)`
- **Native C++ code**: No changes needed
- **JNI method signatures**: No changes needed
- **Public API**: No changes to external interfaces

## Data Models

No data model changes required. This is a configuration fix only.

## Correctness Properties

A property is a characteristic or behavior that should hold true across all valid executions of a system—essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.

### Analysis

After analyzing the acceptance criteria, this bugfix does not lend itself to property-based testing. All requirements are specific, concrete checks:

- Verifying a specific string value ("smart_ffmpeg") is used
- Checking that a specific file (libsmart_ffmpeg.so) exists in the AAR
- Validating that a specific version number (1.0.1) is set
- Testing that class initialization succeeds without exceptions

These are all example-based tests rather than universal properties. Property-based testing is most valuable when testing behaviors that should hold across many different inputs (e.g., "for any valid video file, extracting a thumbnail should return valid RGBA data"). For this bugfix, we're validating specific configuration values and one-time behaviors.

### Testing Approach

Instead of property-based tests, we will use:

1. **Static verification**: Code review to confirm the correct library name is used
2. **Unit tests**: Verify the library name matches between Kotlin and CMake
3. **Integration tests**: Verify the library loads successfully in a test environment
4. **Smoke tests**: Call a native method (getFFmpegVersion) to confirm JNI binding works

## Error Handling

### Current Error (Before Fix)

```
java.lang.UnsatisfiedLinkError: dalvik.system.PathClassLoader[...]
couldn't find "libffmpeg_bridge.so"
```

This error occurs immediately when SmartFfmpegBridge is first accessed, causing application crash.

### Expected Behavior (After Fix)

- Library loads silently and successfully
- No exceptions thrown during class initialization
- Native methods become available for use
- getFFmpegVersion() returns a valid FFmpeg version string

### Error Prevention

To prevent similar issues in the future:

1. **Build-time validation**: Consider adding a Gradle task that verifies the library name in Kotlin matches the CMake target
2. **Documentation**: Document the relationship between CMake target names and System.loadLibrary() parameters
3. **Testing**: Include integration tests that verify library loading in CI/CD pipeline

## Testing Strategy

### Unit Tests

1. **Library Name Consistency Test**
   - Parse CMakeLists.txt to extract the library target name
   - Parse SmartFfmpegBridge.kt to extract the System.loadLibrary() parameter
   - Assert they match (both should be "smart_ffmpeg")
   - **Validates: Requirements 1.1, 1.3**

2. **Version Update Test**
   - Read gradle.properties
   - Assert version is "1.0.1"
   - Assert version follows semantic versioning pattern (X.Y.Z)
   - **Validates: Requirements 2.1, 2.2, 2.3**

### Integration Tests

1. **Library Loading Test**
   - Attempt to load SmartFfmpegBridge class
   - Assert no UnsatisfiedLinkError is thrown
   - **Validates: Requirements 1.2, 3.2**

2. **Native Method Invocation Test**
   - Load SmartFfmpegBridge class
   - Call getFFmpegVersion()
   - Assert result is non-null and non-empty
   - Assert result matches FFmpeg version pattern
   - **Validates: Requirements 1.4, 3.3**

3. **AAR Content Verification Test**
   - Extract the built AAR file
   - Check for presence of libsmart_ffmpeg.so in appropriate architecture folders
   - Assert the file exists and is not empty
   - **Validates: Requirements 3.1**

### Test Configuration

- **Framework**: JUnit 4 or JUnit 5 for unit tests
- **Android Testing**: AndroidX Test for integration tests
- **CI/CD**: All tests should run in the build pipeline before publishing

### Manual Verification

After applying the fix:

1. Build the library: `./gradlew assembleRelease`
2. Extract the AAR and verify libsmart_ffmpeg.so is present
3. Create a test Android app that depends on the library
4. Verify the app starts without crashes
5. Call getFFmpegVersion() and verify it returns a valid version string

## Implementation Notes

### Files to Modify

1. **src/main/kotlin/com/smartmedia/ffmpeg/SmartFfmpegBridge.kt**
   - Line 13: Change "ffmpeg_bridge" to "smart_ffmpeg"

2. **gradle.properties**
   - Update version from 1.0.0 to 1.0.1

### Build Verification

After making changes:

```bash
./gradlew clean
./gradlew assembleRelease
```

Verify the AAR contains the correct library:

```bash
unzip -l build/outputs/aar/smart-ffmpeg-android-release.aar | grep libsmart_ffmpeg.so
```

### Backward Compatibility

This is a breaking change in the sense that version 1.0.0 is non-functional. However, since 1.0.0 crashes immediately on load, there are no existing working integrations to break. Version 1.0.1 will be the first functional release.
