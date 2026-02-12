package com.smartmedia.ffmpeg

import android.graphics.Bitmap
import java.nio.ByteBuffer

/**
 * Bridge class for FFmpeg native operations.
 * Provides high-level API for video thumbnail extraction.
 */
object SmartFfmpegBridge {
    
    init {
        System.loadLibrary("smart_ffmpeg")
    }

    /**
     * Extract thumbnail from video file.
     *
     * @param videoPath Absolute path to video file
     * @param timeMs Time position in milliseconds
     * @param width Target width (will maintain aspect ratio if height is 0)
     * @param height Target height (will maintain aspect ratio if width is 0)
     * @return ByteArray containing RGBA pixel data, or null on error
     */
    @JvmStatic
    external fun extractThumbnail(
        videoPath: String,
        timeMs: Long,
        width: Int,
        height: Int
    ): ByteArray?

    /**
     * Get video duration in milliseconds.
     *
     * @param videoPath Absolute path to video file
     * @return Duration in milliseconds, or -1 on error
     */
    @JvmStatic
    external fun getVideoDuration(videoPath: String): Long

    /**
     * Get video metadata.
     *
     * @param videoPath Absolute path to video file
     * @return Map containing metadata (width, height, duration, codec, etc.)
     */
    @JvmStatic
    external fun getVideoMetadata(videoPath: String): Map<String, Any>?

    /**
     * Get FFmpeg version string.
     *
     * @return FFmpeg version
     */
    @JvmStatic
    external fun getFFmpegVersion(): String
}
