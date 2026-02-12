#!/bin/bash

# FFmpeg Build Script for Android (LGPL-only)
# This script builds FFmpeg for arm64-v8a and armeabi-v7a

set -e

# Configuration
NDK_PATH=${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk-bundle}
FFMPEG_VERSION="6.1"
FFMPEG_SOURCE="ffmpeg-${FFMPEG_VERSION}"
OUTPUT_DIR="$(pwd)/src/main/jniLibs"
INCLUDE_DIR="$(pwd)/src/main/cpp/include"

# Check NDK
if [ ! -d "$NDK_PATH" ]; then
    echo "Error: Android NDK not found at $NDK_PATH"
    echo "Set ANDROID_NDK_HOME environment variable"
    exit 1
fi

# Download FFmpeg if needed
if [ ! -d "$FFMPEG_SOURCE" ]; then
    echo "Downloading FFmpeg ${FFMPEG_VERSION}..."
    wget https://ffmpeg.org/releases/${FFMPEG_SOURCE}.tar.xz
    tar xf ${FFMPEG_SOURCE}.tar.xz
fi

cd $FFMPEG_SOURCE

# Common configure flags (LGPL-only)
COMMON_FLAGS="
    --disable-everything
    --enable-decoder=h264
    --enable-decoder=hevc
    --enable-decoder=mpeg4
    --enable-decoder=vp8
    --enable-decoder=vp9
    --enable-demuxer=mov
    --enable-demuxer=mp4
    --enable-demuxer=matroska
    --enable-demuxer=avi
    --enable-demuxer=flv
    --enable-protocol=file
    --enable-avcodec
    --enable-avformat
    --enable-avutil
    --enable-swscale
    --disable-gpl
    --disable-version3
    --disable-nonfree
    --disable-network
    --disable-encoders
    --disable-muxers
    --disable-filters
    --disable-static
    --enable-shared
    --disable-programs
    --disable-doc
    --disable-debug
    --enable-small
    --enable-optimizations
"

# Build for arm64-v8a
echo "Building for arm64-v8a..."
make clean || true
./configure \
    --prefix=$OUTPUT_DIR/arm64-v8a \
    --enable-cross-compile \
    --target-os=android \
    --arch=aarch64 \
    --cpu=armv8-a \
    --cross-prefix=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android- \
    --sysroot=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/sysroot \
    --cc=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang \
    --cxx=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android21-clang++ \
    $COMMON_FLAGS

make -j$(nproc)
make install

# Copy headers
mkdir -p $INCLUDE_DIR
cp -r $OUTPUT_DIR/arm64-v8a/include/* $INCLUDE_DIR/

# Build for armeabi-v7a
echo "Building for armeabi-v7a..."
make clean
./configure \
    --prefix=$OUTPUT_DIR/armeabi-v7a \
    --enable-cross-compile \
    --target-os=android \
    --arch=arm \
    --cpu=armv7-a \
    --cross-prefix=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/arm-linux-androideabi- \
    --sysroot=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/sysroot \
    --cc=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi21-clang \
    --cxx=$NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/armv7a-linux-androideabi21-clang++ \
    --extra-cflags="-mfloat-abi=softfp -mfpu=neon" \
    $COMMON_FLAGS

make -j$(nproc)
make install

cd ..

echo "FFmpeg build complete!"
echo "Libraries installed in: $OUTPUT_DIR"
echo "Headers installed in: $INCLUDE_DIR"
echo ""
echo "Next steps:"
echo "1. Run: ./gradlew assembleRelease"
echo "2. Check AAR size in build/outputs/aar/"
