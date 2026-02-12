# Keep FFmpeg bridge classes
-keep class com.smartmedia.ffmpeg.** { *; }

# Keep native methods
-keepclasseswithmembernames class * {
    native <methods>;
}
