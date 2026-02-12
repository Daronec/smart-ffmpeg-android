package com.smartmedia.ffmpeg

import org.junit.Test
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Test to verify library name consistency between Kotlin code and CMake configuration.
 * Validates Requirements 1.1, 1.3
 */
class LibraryNameConsistencyTest {

    @Test
    fun `library name in Kotlin matches CMake target`() {
        val expectedLibraryName = "smart_ffmpeg"
        
        // Parse CMakeLists.txt to extract library target name
        val cmakeFile = File("src/main/cpp/CMakeLists.txt")
        assertTrue(cmakeFile.exists(), "CMakeLists.txt should exist")
        
        val cmakeContent = cmakeFile.readText()
        val cmakeLibraryName = extractCMakeLibraryName(cmakeContent)
        
        assertEquals(
            expectedLibraryName,
            cmakeLibraryName,
            "CMake library target should be 'smart_ffmpeg'"
        )
        
        // Parse SmartFfmpegBridge.kt to extract System.loadLibrary parameter
        val kotlinFile = File("src/main/kotlin/com/smartmedia/ffmpeg/SmartFfmpegBridge.kt")
        assertTrue(kotlinFile.exists(), "SmartFfmpegBridge.kt should exist")
        
        val kotlinContent = kotlinFile.readText()
        val kotlinLibraryName = extractKotlinLibraryName(kotlinContent)
        
        assertEquals(
            expectedLibraryName,
            kotlinLibraryName,
            "Kotlin System.loadLibrary should use 'smart_ffmpeg'"
        )
        
        // Verify they match
        assertEquals(
            cmakeLibraryName,
            kotlinLibraryName,
            "Library name in Kotlin must match CMake target name"
        )
    }
    
    private fun extractCMakeLibraryName(content: String): String? {
        // Match: add_library(library_name SHARED ...)
        val regex = Regex("""add_library\s*\(\s*(\w+)\s+SHARED""")
        return regex.find(content)?.groupValues?.get(1)
    }
    
    private fun extractKotlinLibraryName(content: String): String? {
        // Match: System.loadLibrary("library_name")
        val regex = Regex("""System\.loadLibrary\s*\(\s*"(\w+)"\s*\)""")
        return regex.find(content)?.groupValues?.get(1)
    }
}
