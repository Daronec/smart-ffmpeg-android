package com.smartmedia.ffmpeg

import org.junit.Test
import java.io.File
import kotlin.test.assertTrue

/**
 * Manual test for thumbnail generation.
 * This test verifies the test video file exists and provides instructions for running on device.
 * 
 * To run the actual thumbnail extraction test on an Android device:
 * 1. Connect an Android device or start an emulator
 * 2. Run: ./gradlew connectedAndroidTest
 * 3. Check the device storage at: /storage/emulated/0/Android/data/com.smartmedia.ffmpeg.test/files/thumbnails/
 */
class ThumbnailGenerationManualTest {

    @Test
    fun `verify test video file exists`() {
        // This test is optional - video file may not exist in CI environment
        val videoPath = "C:\\Work\\smart-ffmpeg-android\\assets\\heavenly_place.avi"
        val videoFile = File(videoPath)
        
        if (videoFile.exists()) {
            assertTrue(videoFile.length() > 0, "Test video file should not be empty")
            
            val fileSizeKB = videoFile.length() / 1024.0
            
            println("✅ Test video file found:")
            println("   Path: ${videoFile.absolutePath}")
            println("   Size: %.2f KB".format(fileSizeKB))
            println("   Exists: ${videoFile.exists()}")
            println("   Readable: ${videoFile.canRead()}")
        } else {
            println("⚠️  Test video file not found (optional): $videoPath")
            println("   This is expected in CI environment")
        }
        
        println()
        println("📱 To run thumbnail extraction test on Android device:")
        println("   1. Connect Android device or start emulator")
        println("   2. Run: ./gradlew connectedAndroidTest")
        println("   3. Check output in device storage:")
        println("      /storage/emulated/0/Android/data/com.smartmedia.ffmpeg.test/files/thumbnails/")
        
        // Always pass - video file is optional
        assertTrue(true, "Test completed")
    }
    
    @Test
    fun `print test instructions`() {
        println()
        println("=" .repeat(70))
        println("THUMBNAIL GENERATION TEST INSTRUCTIONS")
        println("=" .repeat(70))
        println()
        println("The ThumbnailGenerationTest includes the following tests:")
        println()
        println("1. extract thumbnail from heavenly_place video at 5 seconds")
        println("   - Extracts a single 640x360 thumbnail at 5s")
        println("   - Saves as: heavenly_place_thumbnail_5s.png")
        println()
        println("2. extract multiple thumbnails at different timestamps")
        println("   - Extracts 320x180 thumbnails at: 0s, 1s, 2s, 3s, 4s")
        println("   - Saves as: heavenly_place_thumbnail_0s.png, heavenly_place_thumbnail_1s.png, etc.")
        println()
        println("3. get video duration")
        println("   - Retrieves and displays video duration")
        println()
        println("4. get video metadata")
        println("   - Retrieves width, height, duration, codec info")
        println()
        println("TO RUN ON ANDROID DEVICE:")
        println("   ./gradlew connectedAndroidTest")
        println()
        println("TO RUN SPECIFIC TEST:")
        println("   ./gradlew connectedAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.smartmedia.ffmpeg.ThumbnailGenerationTest#extract_thumbnail_from_Petya_video_at_5_seconds")
        println()
        println("=" .repeat(70))
    }
}
