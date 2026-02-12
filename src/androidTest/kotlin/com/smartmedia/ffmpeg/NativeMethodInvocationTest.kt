package com.smartmedia.ffmpeg

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * Integration test to verify native methods can be invoked successfully.
 * Validates Requirements 1.4, 3.3
 */
@RunWith(AndroidJUnit4::class)
class NativeMethodInvocationTest {

    @Test
    fun testGetFFmpegVersionReturnsValidVersionString() {
        // Call native method to verify JNI binding works
        val version = SmartFfmpegBridge.getFFmpegVersion()
        
        assertNotNull(version, "FFmpeg version should not be null")
        assertTrue(version.isNotEmpty(), "FFmpeg version should not be empty")
        
        // FFmpeg version typically looks like "n4.4.2" or "4.4.2" or similar
        val versionPattern = Regex("""[nN]?\d+\.\d+.*""")
        assertTrue(
            versionPattern.matches(version),
            "FFmpeg version should match pattern (e.g., 'n4.4.2' or '4.4.2'). Got: $version"
        )
    }
    
    @Test
    fun testNativeMethodsAreAccessibleAfterLibraryLoad() {
        // Verify that we can access native method declarations
        // This confirms the JNI bridge is properly established
        
        val methods = SmartFfmpegBridge::class.java.declaredMethods
        val nativeMethodNames = methods
            .filter { java.lang.reflect.Modifier.isNative(it.modifiers) }
            .map { it.name }
            .toSet()
        
        val expectedMethods = setOf(
            "extractThumbnail",
            "getVideoDuration",
            "getVideoMetadata",
            "getFFmpegVersion"
        )
        
        assertTrue(
            nativeMethodNames.containsAll(expectedMethods),
            "All expected native methods should be declared. " +
            "Expected: $expectedMethods, Found: $nativeMethodNames"
        )
    }
}
