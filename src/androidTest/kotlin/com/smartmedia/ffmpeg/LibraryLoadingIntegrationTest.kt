package com.smartmedia.ffmpeg

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import kotlin.test.assertNotNull

/**
 * Integration test to verify the native library loads successfully.
 * Validates Requirements 1.2, 3.2
 */
@RunWith(AndroidJUnit4::class)
class LibraryLoadingIntegrationTest {

    @Test
    fun testSmartFfmpegBridgeLoadsWithoutUnsatisfiedLinkError() {
        // This test will fail if System.loadLibrary() throws UnsatisfiedLinkError
        // Simply accessing the class triggers the companion object init block
        
        var loadingSucceeded = false
        var exception: Throwable? = null
        
        try {
            // Access the class to trigger static initialization
            val bridge = SmartFfmpegBridge::class.java
            assertNotNull(bridge, "SmartFfmpegBridge class should be accessible")
            loadingSucceeded = true
        } catch (e: UnsatisfiedLinkError) {
            exception = e
            loadingSucceeded = false
        } catch (e: ExceptionInInitializerError) {
            exception = e.cause
            loadingSucceeded = false
        }
        
        if (!loadingSucceeded) {
            throw AssertionError(
                "Library loading failed. Expected libsmart_ffmpeg.so to load successfully. " +
                "Error: ${exception?.message}",
                exception
            )
        }
    }
}
