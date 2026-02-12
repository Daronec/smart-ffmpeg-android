package com.smartmedia.ffmpeg

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileOutputStream
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * Integration test for thumbnail generation from video file.
 * Tests the extractThumbnail native method with a real video file.
 */
@RunWith(AndroidJUnit4::class)
class ThumbnailGenerationTest {

    @Test
    fun testExtractThumbnailFromPetyaVideoAt5Seconds() {
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        
        // Path to test video
        val videoPath = "C:\\Work\\smart-ffmpeg-android\\assets\\heavenly_place.avi"
        val videoFile = File(videoPath)
        
        assertTrue(videoFile.exists(), "Test video file should exist at $videoPath")
        assertTrue(videoFile.length() > 0, "Test video file should not be empty")
        
        // Extract thumbnail at 5 seconds (5000ms)
        val timeMs = 5000L
        val targetWidth = 640
        val targetHeight = 360
        
        println("📸 Extracting thumbnail from: ${videoFile.name}")
        println("   Time: ${timeMs}ms (${timeMs / 1000.0}s)")
        println("   Target size: ${targetWidth}x${targetHeight}")
        
        val startTime = System.currentTimeMillis()
        val rgbaData = SmartFfmpegBridge.extractThumbnail(
            videoPath = videoPath,
            timeMs = timeMs,
            width = targetWidth,
            height = targetHeight
        )
        val extractionTime = System.currentTimeMillis() - startTime
        
        assertNotNull(rgbaData, "Thumbnail data should not be null")
        assertTrue(rgbaData.isNotEmpty(), "Thumbnail data should not be empty")
        
        // Expected size: width * height * 4 (RGBA)
        val expectedSize = targetWidth * targetHeight * 4
        assertTrue(
            rgbaData.size == expectedSize,
            "Thumbnail data size should be $expectedSize bytes (${targetWidth}x${targetHeight} RGBA), got ${rgbaData.size}"
        )
        
        println("✅ Thumbnail extracted successfully!")
        println("   Data size: ${rgbaData.size} bytes")
        println("   Extraction time: ${extractionTime}ms")
        
        // Convert RGBA to Bitmap and save to file
        val bitmap = Bitmap.createBitmap(targetWidth, targetHeight, Bitmap.Config.ARGB_8888)
        val buffer = java.nio.ByteBuffer.wrap(rgbaData)
        bitmap.copyPixelsFromBuffer(buffer)
        
        // Save to external storage for manual verification
        val outputDir = File(context.getExternalFilesDir(null), "thumbnails")
        outputDir.mkdirs()
        val outputFile = File(outputDir, "heavenly_place_thumbnail_5s.png")
        
        FileOutputStream(outputFile).use { out ->
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
        }
        
        println("💾 Thumbnail saved to: ${outputFile.absolutePath}")
        
        assertTrue(outputFile.exists(), "Output file should be created")
        assertTrue(outputFile.length() > 0, "Output file should not be empty")
    }
    
    @Test
    fun testExtractMultipleThumbnailsAtDifferentTimestamps() {
        val videoPath = "C:\\Work\\smart-ffmpeg-android\\assets\\heavenly_place.avi"
        val videoFile = File(videoPath)
        
        assertTrue(videoFile.exists(), "Test video file should exist")
        
        val context = InstrumentationRegistry.getInstrumentation().targetContext
        val outputDir = File(context.getExternalFilesDir(null), "thumbnails")
        outputDir.mkdirs()
        
        // Extract thumbnails at different timestamps
        val timestamps = listOf(0L, 1000L, 2000L, 3000L, 4000L) // 0s, 1s, 2s, 3s, 4s
        val targetWidth = 320
        val targetHeight = 180
        
        println("📸 Extracting ${timestamps.size} thumbnails...")
        
        timestamps.forEach { timeMs ->
            println("   Extracting at ${timeMs / 1000.0}s...")
            
            val rgbaData = SmartFfmpegBridge.extractThumbnail(
                videoPath = videoPath,
                timeMs = timeMs,
                width = targetWidth,
                height = targetHeight
            )
            
            assertNotNull(rgbaData, "Thumbnail at ${timeMs}ms should not be null")
            
            val expectedSize = targetWidth * targetHeight * 4
            assertTrue(
                rgbaData.size == expectedSize,
                "Thumbnail at ${timeMs}ms should have correct size"
            )
            
            // Save thumbnail
            val bitmap = Bitmap.createBitmap(targetWidth, targetHeight, Bitmap.Config.ARGB_8888)
            val buffer = java.nio.ByteBuffer.wrap(rgbaData)
            bitmap.copyPixelsFromBuffer(buffer)
            
            val outputFile = File(outputDir, "heavenly_place_thumbnail_${timeMs / 1000}s.png")
            FileOutputStream(outputFile).use { out ->
                bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
            }
            
            println("   ✅ Saved: ${outputFile.name}")
        }
        
        println("✅ All thumbnails extracted successfully!")
        println("💾 Saved to: ${outputDir.absolutePath}")
    }
    
    @Test
    fun testGetVideoDuration() {
        val videoPath = "C:\\Work\\smart-ffmpeg-android\\assets\\heavenly_place.avi"
        val videoFile = File(videoPath)
        
        assertTrue(videoFile.exists(), "Test video file should exist")
        
        val durationMs = SmartFfmpegBridge.getVideoDuration(videoPath)
        
        assertTrue(durationMs > 0, "Video duration should be positive, got $durationMs")
        
        val durationSeconds = durationMs / 1000.0
        val minutes = (durationSeconds / 60).toInt()
        val seconds = (durationSeconds % 60).toInt()
        
        println("⏱️ Video duration: ${durationMs}ms (${minutes}m ${seconds}s)")
    }
    
    @Test
    fun testGetVideoMetadata() {
        val videoPath = "C:\\Work\\smart-ffmpeg-android\\assets\\heavenly_place.avi"
        val videoFile = File(videoPath)
        
        assertTrue(videoFile.exists(), "Test video file should exist")
        
        val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)
        
        assertNotNull(metadata, "Video metadata should not be null")
        assertTrue(metadata.isNotEmpty(), "Video metadata should not be empty")
        
        println("📊 Video metadata:")
        metadata.forEach { (key, value) ->
            println("   $key: $value")
        }
        
        // Verify expected metadata fields exist
        assertTrue(metadata.containsKey("width"), "Metadata should contain width")
        assertTrue(metadata.containsKey("height"), "Metadata should contain height")
        assertTrue(metadata.containsKey("duration"), "Metadata should contain duration")
    }
}
