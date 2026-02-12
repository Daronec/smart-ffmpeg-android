package com.smartmedia.ffmpeg

import android.view.Surface

class SmartFFmpegPlayer {
    
    private var nativeHandle: Long = 0
    private var eventCallback: EventCallback? = null
    
    interface EventCallback {
        fun onPrepared(hasAudio: Boolean, durationMs: Long)
        fun onSurfaceReady()
        fun onFirstFrame()
        fun onFirstFrameAfterSeek()
        fun onPosition(positionMs: Long)
        fun onEnded()
        fun onError(message: String)
        fun onAudioStateChanged(state: String)
    }
    
    init {
        System.loadLibrary("smart_ffmpeg")
    }
    
    fun setEventCallback(callback: EventCallback) {
        this.eventCallback = callback
    }
    
    fun prepare(path: String): Boolean {
        nativeHandle = nativePrepare(path)
        return nativeHandle != 0L
    }
    
    fun setSurface(surface: Surface?) {
        if (nativeHandle != 0L) {
            nativeSetSurface(nativeHandle, surface)
        }
    }
    
    fun play() {
        if (nativeHandle != 0L) {
            nativePlay(nativeHandle)
        }
    }
    
    fun pause() {
        if (nativeHandle != 0L) {
            nativePause(nativeHandle)
        }
    }
    
    fun seekTo(positionMs: Long, exact: Boolean = false) {
        if (nativeHandle != 0L) {
            nativeSeek(nativeHandle, positionMs / 1000.0, exact)
        }
    }
    
    fun getPosition(): Long {
        return if (nativeHandle != 0L) {
            nativeGetPosition(nativeHandle)
        } else 0L
    }
    
    fun getDuration(): Long {
        return if (nativeHandle != 0L) {
            nativeGetDuration(nativeHandle)
        } else 0L
    }
    
    fun setSpeed(speed: Float) {
        if (nativeHandle != 0L) {
            nativeSetSpeed(nativeHandle, speed.toDouble())
        }
    }
    
    fun release() {
        if (nativeHandle != 0L) {
            nativeRelease(nativeHandle)
            nativeHandle = 0
        }
    }
    
    // JNI callbacks (вызываются из native кода)
    @Suppress("unused")
    private fun onPreparedCallback(hasAudio: Boolean, durationMs: Long) {
        eventCallback?.onPrepared(hasAudio, durationMs)
    }
    
    @Suppress("unused")
    private fun onSurfaceReadyCallback() {
        eventCallback?.onSurfaceReady()
    }
    
    @Suppress("unused")
    private fun onFirstFrameCallback() {
        eventCallback?.onFirstFrame()
    }
    
    @Suppress("unused")
    private fun onFirstFrameAfterSeekCallback() {
        eventCallback?.onFirstFrameAfterSeek()
    }
    
    @Suppress("unused")
    private fun onPositionCallback(positionMs: Long) {
        eventCallback?.onPosition(positionMs)
    }
    
    @Suppress("unused")
    private fun onEndedCallback() {
        eventCallback?.onEnded()
    }
    
    @Suppress("unused")
    private fun onErrorCallback(message: String) {
        eventCallback?.onError(message)
    }
    
    @Suppress("unused")
    private fun onAudioStateChangedCallback(state: String) {
        eventCallback?.onAudioStateChanged(state)
    }
    
    // Native methods
    private external fun nativePrepare(path: String): Long
    private external fun nativeSetSurface(handle: Long, surface: Surface?)
    private external fun nativePlay(handle: Long)
    private external fun nativePause(handle: Long)
    private external fun nativeSeek(handle: Long, seconds: Double, exact: Boolean)
    private external fun nativeGetPosition(handle: Long): Long
    private external fun nativeGetDuration(handle: Long): Long
    private external fun nativeSetSpeed(handle: Long, speed: Double)
    private external fun nativeRelease(handle: Long)
}
