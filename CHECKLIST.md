# Smart FFmpeg Android - Build & Publish Checklist

## Pre-Build Checklist

### Environment Setup

- [ ] Android NDK r26+ installed
- [ ] `ANDROID_NDK_HOME` environment variable set
- [ ] Java 11+ installed
- [ ] Build tools installed (yasm, nasm, pkg-config)
- [ ] Git configured

### Project Structure

- [ ] `build.gradle` configured correctly
- [ ] `settings.gradle` configured correctly
- [ ] `gradle.properties` has correct settings
- [ ] `CMakeLists.txt` paths are correct
- [ ] `AndroidManifest.xml` exists (minimal)

## FFmpeg Build Checklist

### Configuration

- [ ] FFmpeg version selected (6.1 recommended)
- [ ] Configure flags are LGPL-only
- [ ] No GPL codecs enabled
- [ ] No GPL filters enabled
- [ ] `--disable-gpl` flag present
- [ ] `--enable-small` flag present
- [ ] Only required decoders enabled (h264, hevc, mpeg4)
- [ ] Only required demuxers enabled (mov, mp4, matroska)

### Build Process

- [ ] Run `./build_ffmpeg.sh` successfully
- [ ] arm64-v8a libraries generated
- [ ] armeabi-v7a libraries generated
- [ ] Headers copied to `src/main/cpp/include/`
- [ ] Libraries copied to `src/main/jniLibs/`

### Verification

- [ ] Check library sizes (should be ~7-8MB per arch)
- [ ] Verify LGPL compliance (`ffmpeg -version`)
- [ ] Test libraries load correctly
- [ ] Check symbols with `nm`

## AAR Build Checklist

### Build

- [ ] Run `./gradlew clean`
- [ ] Run `./gradlew assembleRelease`
- [ ] AAR generated in `build/outputs/aar/`
- [ ] AAR size under 15MB

### AAR Contents Verification

- [ ] Extract AAR: `unzip smart-ffmpeg-android-release.aar -d test`
- [ ] Check `jni/arm64-v8a/` contains all .so files
- [ ] Check `jni/armeabi-v7a/` contains all .so files
- [ ] Check `classes.jar` contains Kotlin bridge classes
- [ ] Check `AndroidManifest.xml` is present

### Testing

- [ ] Create test Android app
- [ ] Add AAR as dependency
- [ ] Test `extractThumbnail()` method
- [ ] Test `getVideoDuration()` method
- [ ] Test `getVideoMetadata()` method
- [ ] Test `getFFmpegVersion()` method
- [ ] Test on real device (arm64-v8a)
- [ ] Test on real device (armeabi-v7a)

## Publishing Checklist

### GitHub Setup

- [ ] GitHub repository created
- [ ] Personal Access Token generated (write:packages)
- [ ] Token saved in `~/.gradle/gradle.properties`
- [ ] Repository URL updated in `build.gradle`

### Pre-Publish

- [ ] Version number updated in `build.gradle`
- [ ] CHANGELOG.md updated
- [ ] README.md updated
- [ ] All tests passing
- [ ] No uncommitted changes

### Publish

- [ ] Run `./gradlew publish`
- [ ] Verify package appears in GitHub Packages
- [ ] Test installation from GitHub Packages
- [ ] Create GitHub release
- [ ] Tag version in git

## Integration Checklist

### Flutter Plugin Integration

- [ ] Update Flutter plugin `build.gradle`
- [ ] Add GitHub Packages repository
- [ ] Add dependency on smart-ffmpeg
- [ ] Update plugin Kotlin code
- [ ] Remove old FFmpeg files
- [ ] Test Flutter plugin
- [ ] Update Flutter plugin documentation

### Documentation

- [ ] README.md complete
- [ ] BUILDING.md complete
- [ ] INTEGRATION.md complete
- [ ] API documentation complete
- [ ] Example code provided
- [ ] License information clear

## Quality Checklist

### Code Quality

- [ ] No hardcoded paths
- [ ] Proper error handling
- [ ] Memory management correct
- [ ] No memory leaks
- [ ] Thread-safe operations
- [ ] Proper JNI cleanup

### Performance

- [ ] Thumbnail extraction < 100ms
- [ ] Memory usage reasonable
- [ ] No ANR issues
- [ ] Efficient resource usage

### Security

- [ ] No buffer overflows
- [ ] Input validation
- [ ] Path traversal prevention
- [ ] No hardcoded credentials

## Compliance Checklist

### License

- [ ] LGPL 2.1 license file present
- [ ] FFmpeg attribution included
- [ ] No GPL code included
- [ ] Source code availability documented

### Legal

- [ ] No patent-encumbered codecs (if applicable)
- [ ] Export compliance checked
- [ ] Third-party licenses documented

## Maintenance Checklist

### Regular Updates

- [ ] Check for FFmpeg security updates
- [ ] Update to latest stable FFmpeg
- [ ] Update Android dependencies
- [ ] Update Gradle version
- [ ] Update Kotlin version

### Monitoring

- [ ] Monitor GitHub issues
- [ ] Monitor crash reports
- [ ] Monitor performance metrics
- [ ] Monitor package downloads

## Release Checklist

### Pre-Release

- [ ] All checklists above completed
- [ ] Version bumped
- [ ] CHANGELOG updated
- [ ] Documentation updated
- [ ] Tests passing

### Release

- [ ] Create git tag
- [ ] Push to GitHub
- [ ] Publish to GitHub Packages
- [ ] Create GitHub release
- [ ] Announce release

### Post-Release

- [ ] Monitor for issues
- [ ] Update dependent projects
- [ ] Update documentation site
- [ ] Notify users

## Troubleshooting Checklist

### Build Issues

- [ ] Check NDK version
- [ ] Check disk space
- [ ] Check build tools installed
- [ ] Check environment variables
- [ ] Clean build directory

### Runtime Issues

- [ ] Check ABI compatibility
- [ ] Check library loading
- [ ] Check JNI signatures
- [ ] Check file permissions
- [ ] Check Android version compatibility

### Publishing Issues

- [ ] Check GitHub token permissions
- [ ] Check repository URL
- [ ] Check network connectivity
- [ ] Check Gradle configuration
- [ ] Check version conflicts

## Notes

- Keep this checklist updated
- Check off items as you complete them
- Document any deviations
- Share learnings with team
