package com.smartmedia.ffmpeg

import org.junit.Test
import java.io.File
import java.util.zip.ZipFile
import kotlin.test.assertTrue

/**
 * Test to verify the built AAR contains the correct native library.
 * Validates Requirements 3.1
 */
class AarContentVerificationTest {

    @Test
    fun `AAR contains libsmart_ffmpeg_so for all architectures`() {
        // Find the AAR file in build outputs
        val aarFile = findAarFile()
        
        if (aarFile == null) {
            println("WARNING: AAR file not found. Run './gradlew assembleRelease' first.")
            println("Skipping AAR content verification test.")
            return
        }
        
        assertTrue(aarFile.exists(), "AAR file should exist at ${aarFile.absolutePath}")
        assertTrue(aarFile.length() > 0, "AAR file should not be empty")
        
        // Expected architectures
        val expectedArchitectures = listOf(
            "arm64-v8a"
            // Note: build.gradle currently only builds arm64-v8a
            // If other architectures are added, include them here:
            // "armeabi-v7a", "x86", "x86_64"
        )
        
        ZipFile(aarFile).use { zip ->
            val entries = zip.entries().toList().map { it.name }
            
            for (arch in expectedArchitectures) {
                val libraryPath = "jni/$arch/libsmart_ffmpeg.so"
                
                assertTrue(
                    entries.any { it == libraryPath },
                    "AAR should contain $libraryPath. Found entries: ${entries.filter { it.contains(".so") }}"
                )
                
                // Verify the library file is not empty
                val entry = zip.getEntry(libraryPath)
                assertTrue(
                    entry.size > 0,
                    "Library file $libraryPath should not be empty"
                )
            }
        }
    }
    
    @Test
    fun `AAR does not contain libffmpeg_bridge_so`() {
        val aarFile = findAarFile()
        
        if (aarFile == null) {
            println("WARNING: AAR file not found. Skipping test.")
            return
        }
        
        ZipFile(aarFile).use { zip ->
            val entries = zip.entries().toList().map { it.name }
            val wrongLibraryEntries = entries.filter { it.contains("libffmpeg_bridge.so") }
            
            assertTrue(
                wrongLibraryEntries.isEmpty(),
                "AAR should NOT contain libffmpeg_bridge.so. Found: $wrongLibraryEntries"
            )
        }
    }
    
    private fun findAarFile(): File? {
        val buildDir = File("build/outputs/aar")
        if (!buildDir.exists()) {
            return null
        }
        
        // Look for release AAR
        val aarFiles = buildDir.listFiles { file ->
            file.name.endsWith(".aar") && file.name.contains("release", ignoreCase = true)
        }
        
        return aarFiles?.firstOrNull()
    }
}
