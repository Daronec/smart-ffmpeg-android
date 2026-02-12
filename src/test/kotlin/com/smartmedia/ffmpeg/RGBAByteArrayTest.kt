package com.smartmedia.ffmpeg

import org.junit.Test
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * Unit tests for RGBA byte array manipulation and validation.
 * These tests run on JVM without requiring Android device.
 */
class RGBAByteArrayTest {

    @Test
    fun testCreateRGBAByteArray() {
        val width = 100
        val height = 100
        val rgbaData = ByteArray(width * height * 4)
        
        assertEquals(40000, rgbaData.size, "100x100 RGBA should be 40,000 bytes")
        assertNotNull(rgbaData, "Byte array should not be null")
    }
    
    @Test
    fun testRGBAPixelAccess() {
        val width = 10
        val height = 10
        val rgbaData = ByteArray(width * height * 4)
        
        // Set pixel at (5, 5) to red (255, 0, 0, 255)
        val x = 5
        val y = 5
        val pixelIndex = (y * width + x) * 4
        
        rgbaData[pixelIndex + 0] = 255.toByte()  // R
        rgbaData[pixelIndex + 1] = 0             // G
        rgbaData[pixelIndex + 2] = 0             // B
        rgbaData[pixelIndex + 3] = 255.toByte()  // A
        
        // Verify pixel
        assertEquals(255.toByte(), rgbaData[pixelIndex + 0], "Red channel should be 255")
        assertEquals(0.toByte(), rgbaData[pixelIndex + 1], "Green channel should be 0")
        assertEquals(0.toByte(), rgbaData[pixelIndex + 2], "Blue channel should be 0")
        assertEquals(255.toByte(), rgbaData[pixelIndex + 3], "Alpha channel should be 255")
    }
    
    @Test
    fun testRGBAToARGBConversion() {
        // RGBA format: R, G, B, A
        val rgbaData = byteArrayOf(
            255.toByte(), 0, 0, 255.toByte(),  // Red pixel
            0, 255.toByte(), 0, 255.toByte(),  // Green pixel
            0, 0, 255.toByte(), 255.toByte()   // Blue pixel
        )
        
        // Convert RGBA to ARGB (Android Bitmap format)
        val argbData = IntArray(3)
        for (i in 0 until 3) {
            val r = rgbaData[i * 4 + 0].toInt() and 0xFF
            val g = rgbaData[i * 4 + 1].toInt() and 0xFF
            val b = rgbaData[i * 4 + 2].toInt() and 0xFF
            val a = rgbaData[i * 4 + 3].toInt() and 0xFF
            
            argbData[i] = (a shl 24) or (r shl 16) or (g shl 8) or b
        }
        
        // Verify conversions
        assertEquals(0xFFFF0000.toInt(), argbData[0], "Red pixel in ARGB")
        assertEquals(0xFF00FF00.toInt(), argbData[1], "Green pixel in ARGB")
        assertEquals(0xFF0000FF.toInt(), argbData[2], "Blue pixel in ARGB")
    }
    
    @Test
    fun testByteArraySizeForCommonResolutions() {
        val resolutions = mapOf(
            "QVGA" to Pair(320, 240),
            "VGA" to Pair(640, 480),
            "HD" to Pair(1280, 720),
            "Full HD" to Pair(1920, 1080),
            "4K" to Pair(3840, 2160)
        )
        
        println()
        println("RGBA Byte Array Sizes for Common Resolutions:")
        println("=" .repeat(60))
        
        resolutions.forEach { (name, resolution) ->
            val (width, height) = resolution
            val bytes = width * height * 4
            val kb = bytes / 1024.0
            val mb = kb / 1024.0
            
            println("$name (${width}x${height}): ${bytes} bytes (%.2f KB, %.2f MB)".format(kb, mb))
            
            assertTrue(bytes > 0, "$name should have positive byte size")
        }
        
        println("=" .repeat(60))
    }
    
    @Test
    fun testValidateByteArrayNotNull() {
        val width = 640
        val height = 360
        
        // Simulate what extractThumbnail should return
        val rgbaData: ByteArray? = ByteArray(width * height * 4)
        
        assertNotNull(rgbaData, "RGBA data should not be null")
        assertTrue(rgbaData.isNotEmpty(), "RGBA data should not be empty")
        assertEquals(921600, rgbaData.size, "Size should match width * height * 4")
    }
    
    @Test
    fun testByteToUnsignedInt() {
        // Bytes in Java/Kotlin are signed (-128 to 127)
        // Need to convert to unsigned (0 to 255) for color values
        
        val signedByte: Byte = 255.toByte()  // Actually -1 in signed
        val unsignedInt = signedByte.toInt() and 0xFF
        
        assertEquals(255, unsignedInt, "255 as byte should convert to 255 as unsigned int")
        
        val testByte: Byte = 128.toByte()  // Actually -128 in signed
        val testInt = testByte.toInt() and 0xFF
        
        assertEquals(128, testInt, "128 as byte should convert to 128 as unsigned int")
    }
    
    @Test
    fun testFillRGBAWithGradient() {
        val width = 256
        val height = 1
        val rgbaData = ByteArray(width * height * 4)
        
        // Create horizontal gradient from black to white
        for (x in 0 until width) {
            val pixelIndex = x * 4
            val value = x.toByte()
            
            rgbaData[pixelIndex + 0] = value  // R
            rgbaData[pixelIndex + 1] = value  // G
            rgbaData[pixelIndex + 2] = value  // B
            rgbaData[pixelIndex + 3] = 255.toByte()  // A
        }
        
        // Verify first pixel (black)
        assertEquals(0.toByte(), rgbaData[0])
        
        // Verify last pixel (white-ish, 255)
        val lastPixelIndex = (width - 1) * 4
        assertEquals(255.toByte(), rgbaData[lastPixelIndex])
        
        println("✅ Created ${width}x${height} gradient (${rgbaData.size} bytes)")
    }
}
