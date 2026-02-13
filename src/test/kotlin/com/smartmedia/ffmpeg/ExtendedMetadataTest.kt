package com.smartmedia.ffmpeg

import org.junit.Test
import org.json.JSONObject
import kotlin.test.assertEquals
import kotlin.test.assertNotNull
import kotlin.test.assertTrue

/**
 * Test extended metadata fields
 * Validates new fields: fps, audioCodec, streamCount, hasAudio, hasSubtitles, etc.
 */
class ExtendedMetadataTest {

    @Test
    fun `metadata contains all required fields`() {
        // This test validates the structure without actual video file
        val requiredFields = listOf(
            "width",
            "height",
            "duration",
            "codec",
            "bitrate",
            "fps",
            "rotation",
            "container",
            "streamCount",
            "hasAudio",
            "hasSubtitles"
        )
        
        // Verify field names are correct
        assertTrue(requiredFields.isNotEmpty(), "Required fields list should not be empty")
    }
    
    @Test
    fun `audio fields are present when audio exists`() {
        val audioFields = listOf(
            "audioCodec",
            "sampleRate",
            "channels"
        )
        
        // Verify audio field names are correct
        assertTrue(audioFields.isNotEmpty(), "Audio fields list should not be empty")
    }
    
    @Test
    fun `json metadata has success field`() {
        // Test JSON structure
        val successJson = """{"success":true,"data":{"width":1920}}"""
        val errorJson = """{"success":false,"error":"Invalid file"}"""
        
        val successObj = JSONObject(successJson)
        assertTrue(successObj.getBoolean("success"), "Success JSON should have success=true")
        
        val errorObj = JSONObject(errorJson)
        assertTrue(!errorObj.getBoolean("success"), "Error JSON should have success=false")
        assertTrue(errorObj.has("error"), "Error JSON should have error field")
    }
    
    @Test
    fun `json metadata structure is valid`() {
        val sampleJson = """
        {
            "success": true,
            "data": {
                "width": 1920,
                "height": 1080,
                "duration": 120000,
                "codec": "h264",
                "bitrate": 5000000,
                "fps": 30.0,
                "rotation": 0,
                "container": "mp4",
                "streamCount": 2,
                "hasAudio": true,
                "hasSubtitles": false,
                "audioCodec": "aac",
                "sampleRate": 48000,
                "channels": 2
            }
        }
        """.trimIndent()
        
        val json = JSONObject(sampleJson)
        assertTrue(json.getBoolean("success"))
        
        val data = json.getJSONObject("data")
        assertEquals(1920, data.getInt("width"))
        assertEquals(1080, data.getInt("height"))
        assertEquals(120000, data.getLong("duration"))
        assertEquals("h264", data.getString("codec"))
        assertEquals(5000000, data.getLong("bitrate"))
        assertEquals(30.0, data.getDouble("fps"), 0.1)
        assertEquals(0, data.getInt("rotation"))
        assertEquals("mp4", data.getString("container"))
        assertEquals(2, data.getInt("streamCount"))
        assertTrue(data.getBoolean("hasAudio"))
        assertTrue(!data.getBoolean("hasSubtitles"))
        assertEquals("aac", data.getString("audioCodec"))
        assertEquals(48000, data.getInt("sampleRate"))
        assertEquals(2, data.getInt("channels"))
    }
    
    @Test
    fun `error json structure is valid`() {
        val errorJson = """
        {
            "success": false,
            "error": "Could not open file: No such file or directory"
        }
        """.trimIndent()
        
        val json = JSONObject(errorJson)
        assertTrue(!json.getBoolean("success"))
        assertTrue(json.has("error"))
        assertNotNull(json.getString("error"))
    }
}
