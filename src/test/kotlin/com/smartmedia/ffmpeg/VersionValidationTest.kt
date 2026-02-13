package com.smartmedia.ffmpeg

import org.junit.Test
import java.io.File
import kotlin.test.assertEquals
import kotlin.test.assertTrue

/**
 * Test to verify version number is correctly set.
 * Validates Requirements 2.1, 2.2, 2.3
 */
class VersionValidationTest {

    @Test
    fun `version is set to 1_0_2`() {
        val expectedVersion = "1.0.5"
        
        val buildGradleFile = File("build.gradle")
        assertTrue(buildGradleFile.exists(), "build.gradle should exist")
        
        val content = buildGradleFile.readText()
        val version = extractVersion(content)
        
        assertEquals(
            expectedVersion,
            version,
            "Version should be 1.0.5"
        )
    }
    
    @Test
    fun `version follows semantic versioning pattern`() {
        val buildGradleFile = File("build.gradle")
        assertTrue(buildGradleFile.exists(), "build.gradle should exist")
        
        val content = buildGradleFile.readText()
        val version = extractVersion(content)
        
        val semverPattern = Regex("""^\d+\.\d+\.\d+$""")
        assertTrue(
            version != null && semverPattern.matches(version),
            "Version should follow semantic versioning pattern (X.Y.Z)"
        )
    }
    
    private fun extractVersion(content: String): String? {
        // Match: version = '1.0.2'
        val regex = Regex("""version\s*=\s*['"]([^'"]+)['"]""")
        return regex.find(content)?.groupValues?.get(1)
    }
}
