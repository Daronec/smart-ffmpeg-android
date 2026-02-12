cmake_minimum_required(VERSION 3.22.1)
project(smart_ffmpeg)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# Путь к нативному движку
set(NATIVE_ENGINE_DIR ${CMAKE_SOURCE_DIR}/native_media_engine)

# Включаем заголовки FFmpeg
include_directories(
    ${NATIVE_ENGINE_DIR}/include
    ${NATIVE_ENGINE_DIR}/ffmpeg_player
)

# Исходники нативного плеера
file(GLOB FFMPEG_PLAYER_SOURCES
    ${NATIVE_ENGINE_DIR}/ffmpeg_player/*.c
)

# Библиотека: FFmpeg Player (для воспроизведения видео)
add_library(smart_ffmpeg SHARED
    ${FFMPEG_PLAYER_SOURCES}
)

target_link_libraries(smart_ffmpeg
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libavcodec.so
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libavformat.so
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libavutil.so
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libswscale.so
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libswresample.so
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libavfilter.so
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libavdevice.so
    android
    log
    GLESv2
    EGL
    OpenSLES
)
