#include "audio_render_android.h"
#include "avsync_gate.h"  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION
#include "ffmpeg_player.h"  // 🔥 КРИТИЧЕСКИЙ FIX: для AudioStateEnum (AUDIO_DEAD, AUDIO_READY, etc.)
#include <android/log.h>
#include <string.h>

#define LOG_TAG "AudioRender"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/// Получить JNIEnv из JavaVM
static JNIEnv *get_env(JavaVM *jvm) {
    JNIEnv *env = NULL;
    if ((*jvm)->GetEnv(jvm, (void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    }
    return env;
}

bool audio_render_init(AudioRenderAndroid *ar,
                       JavaVM *jvm,
                       int sample_rate,
                       int channels) {
    memset(ar, 0, sizeof(*ar));
    
    ar->jvm = jvm;
    ar->sample_rate = sample_rate;
    ar->channels = channels;
    ar->bytes_per_sample = 2; // PCM 16-bit
    
    JNIEnv *env = get_env(jvm);
    if (!env) {
        LOGE("Failed to get JNIEnv");
        return false;
    }
    
    // Находим класс AudioTrack
    jclass at_cls = (*env)->FindClass(env, "android/media/AudioTrack");
    if (!at_cls) {
        LOGE("AudioTrack class not found");
        return false;
    }
    
    // Получаем минимальный размер буфера
    // static int getMinBufferSize(int sampleRateInHz, int channelConfig, int audioFormat)
    jmethodID get_min_buf = (*env)->GetStaticMethodID(
        env, at_cls, "getMinBufferSize",
        "(III)I"
    );
    
    if (!get_min_buf) {
        LOGE("getMinBufferSize method not found");
        (*env)->DeleteLocalRef(env, at_cls);
        return false;
    }
    
    // Конфигурация каналов
    // CHANNEL_OUT_MONO = 4, CHANNEL_OUT_STEREO = 12
    int channel_config = (channels == 1) ? 4 : 12;
    int audio_format = 2; // ENCODING_PCM_16BIT
    
    jint min_buf = (*env)->CallStaticIntMethod(
        env, at_cls, get_min_buf,
        sample_rate, channel_config, audio_format
    );
    
    if (min_buf <= 0) {
        LOGE("Invalid min buffer size: %d", min_buf);
        (*env)->DeleteLocalRef(env, at_cls);
        return false;
    }
    
    // Конструктор AudioTrack
    // AudioTrack(streamType, sampleRateInHz, channelConfig, audioFormat, bufferSizeInBytes, mode)
    jmethodID ctor = (*env)->GetMethodID(
        env, at_cls, "<init>",
        "(IIIIII)V"
    );
    
    if (!ctor) {
        LOGE("AudioTrack constructor not found");
        (*env)->DeleteLocalRef(env, at_cls);
        return false;
    }
    
    // Шаг 31.4: Low-latency AudioTrack config
    // bufferSize = min * 2 (не больше, чтобы минимизировать лаг)
    int buffer_size = min_buf * 2;
    
    // Создаём AudioTrack
    // STREAM_MUSIC = 3, MODE_STREAM = 1
    jobject track = (*env)->NewObject(
        env, at_cls, ctor,
        3,              // STREAM_MUSIC
        sample_rate,
        channel_config,
        audio_format,
        buffer_size,    // Шаг 31.4: оптимальный размер буфера
        1               // MODE_STREAM
    );
    
    if (!track) {
        LOGE("Failed to create AudioTrack");
        (*env)->DeleteLocalRef(env, at_cls);
        return false;
    }
    
    // Сохраняем global ref
    ar->audio_track = (*env)->NewGlobalRef(env, track);
    (*env)->DeleteLocalRef(env, track);
    
    // Получаем method IDs
    ar->write_mid = (*env)->GetMethodID(env, at_cls, "write", "([BII)I");
    ar->play_mid = (*env)->GetMethodID(env, at_cls, "play", "()V");
    ar->pause_mid = (*env)->GetMethodID(env, at_cls, "pause", "()V");
    ar->stop_mid = (*env)->GetMethodID(env, at_cls, "stop", "()V");
    ar->release_mid = (*env)->GetMethodID(env, at_cls, "release", "()V");
    ar->get_play_state_mid = (*env)->GetMethodID(env, at_cls, "getPlayState", "()I");
    
    // Шаг 26.1: getPlaybackHeadPosition для точного audio clock
    // Сохраняем method ID для getPlaybackHeadPosition (если нужно)
    // Пока используем samples_written для расчёта clock
    
    (*env)->DeleteLocalRef(env, at_cls);
    
    if (!ar->write_mid || !ar->play_mid || !ar->pause_mid || 
        !ar->stop_mid || !ar->release_mid || !ar->get_play_state_mid) {
        LOGE("Failed to get AudioTrack method IDs");
        audio_render_release(ar);
        return false;
    }
    
    LOGI("AudioTrack initialized (%d Hz, %d ch, buffer=%d bytes) - Low-latency",
         sample_rate, channels, buffer_size);
    
    return true;
}

void audio_render_start(AudioRenderAndroid *ar) {
    if (!ar->audio_track || ar->started) {
        return;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        LOGE("Failed to get JNIEnv in audio_render_start");
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIOFOCUS ASSERT - проверяем audio focus перед AudioTrack.play()
    // На некоторых устройствах (Huawei/HiSilicon) AudioTrack может быть остановлен системой
    // если audio focus не получен или громкость = 0%
    // Это не ошибка, но важно для диагностики silent playback
    LOGI("🔊 AUDIOFOCUS ASSERT: AudioTrack.play() called (audio focus should be requested by app)");
    LOGI("   If audio focus not gained, AudioTrack may be stopped by Android AudioSystem");
    LOGI("   Error -2103464049 (onAudioException) indicates system stopped AudioTrack");
    
    (*env)->CallVoidMethod(env, ar->audio_track, ar->play_mid);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("❌ Exception in AudioTrack.play() - AudioSystem may have stopped AudioTrack");
        LOGE("   This can happen if: volume=0%%, audio focus not gained, or OEM policy");
        (*env)->ExceptionClear(env);
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 5️⃣ AUDIO_STOPPED_BY_SYSTEM
        // onAudioException при AudioTrack.play() → AudioTrack остановлен системой
        // Нужен доступ к PlayerContext для обновления audio_state
        // (Это будет обработано в audio_render_thread при обнаружении frozen playbackHead)
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO-NATIVE Contract - проверяем getPlayState() == PLAYSTATE_PLAYING
    // audio_clock updates ≠ playing
    // Clock может тикать даже если AudioTrack не вышел в PLAYSTATE_PLAYING
    // ТОЛЬКО после getPlayState() == PLAYSTATE_PLAYING можно говорить, что аудио реально играет
    int play_state = (*env)->CallIntMethod(env, ar->audio_track, ar->get_play_state_mid);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("❌ Exception in AudioTrack.getPlayState()");
        (*env)->ExceptionClear(env);
        return;
    }
    
    // PLAYSTATE_PLAYING = 3 (android.media.AudioTrack)
    const int PLAYSTATE_PLAYING = 3;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO-ASSERTS A1 - ASSERT: AudioTrack.play() ПРИНЯТ
    // ❌ БЕЗ ЭТОГО события Flutter НЕ ИМЕЕТ ПРАВА доверять audio_clock
    if (play_state != PLAYSTATE_PLAYING) {
        LOGE("❌ AUDIO_ASSERT A1 FAILED: AudioTrack.getPlayState() = %d (expected PLAYSTATE_PLAYING=3)", play_state);
        LOGE("   AudioTrack.play() called but state is not PLAYING");
        LOGE("   This means audio will NOT be heard, even if audio_clock updates");
        LOGE("   FATAL: Cannot continue playback without valid AudioTrack state");
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Эмитим ERROR состояние
        if (ar->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)ar->player_ctx;
            ctx->audio_state = AUDIO_DEAD; // Терминальное состояние
            extern void native_player_emit_audio_state_event(const char *state);
            native_player_emit_audio_state_event("dead");
        }
        
        // НЕ устанавливаем ar->started = true, так как AudioTrack не в PLAYING
        // ❌ FATAL: Не продолжаем playback без валидного AudioTrack
        return;
    }
    
    ar->started = true;
    LOGI("✅ AudioTrack.play() accepted: getPlayState() == PLAYSTATE_PLAYING");
    LOGI("   Audio is now REAL playing (not just clock updates)");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 4️⃣ AUDIO_PLAYING
    // AudioTrack.play() ВЫЗВАН И ПРИНЯТ (getPlayState() == PLAYSTATE_PLAYING)
    // Эмитим audioStarted событие ТОЛЬКО здесь
    if (ar->player_ctx) {
        PlayerContext *ctx = (PlayerContext *)ar->player_ctx;
        // Переход в AUDIO_PLAYING только из AUDIO_READY (buffer primed)
        if (ctx->audio_state == AUDIO_READY) {
            ctx->audio_state = AUDIO_PLAYING;
            ALOGI("🎧 AudioState: AUDIO_READY → AUDIO_PLAYING (AudioTrack.getPlayState() == PLAYSTATE_PLAYING)");
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - НЕ устанавливаем AVSYNC valid здесь
            // AVSYNC valid будет установлен только после первого успешного write() когда clock_valid = true
            // Это гарантирует, что AVSYNC использует валидный PTS-based clock, а не playbackHeadPosition
            
            extern void native_player_emit_audio_state_event(const char *state);
            native_player_emit_audio_state_event("playing");
        } else if (ctx->audio_state == AUDIO_INITIALIZED) {
            // Если ещё не готов (buffer не primed), ждём AUDIO_READY
            LOGI("🎧 AudioState: AUDIO_INITIALIZED (waiting for buffer primed → AUDIO_READY)");
        }
    }
}

void audio_render_pause(AudioRenderAndroid *ar) {
    if (!ar->audio_track || !ar->started) {
        return;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return;
    }
    
    (*env)->CallVoidMethod(env, ar->audio_track, ar->pause_mid);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("Exception in AudioTrack.pause()");
        (*env)->ExceptionClear(env);
    }
    
    ar->started = false;
    LOGI("AudioTrack paused");
}

void audio_render_stop(AudioRenderAndroid *ar) {
    if (!ar->audio_track) {
        return;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return;
    }
    
    (*env)->CallVoidMethod(env, ar->audio_track, ar->stop_mid);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("Exception in AudioTrack.stop()");
        (*env)->ExceptionClear(env);
    }
    
    ar->started = false;
    LOGI("AudioTrack stopped");
}

int audio_render_write(AudioRenderAndroid *ar,
                       const uint8_t *data,
                       int size) {
    if (!ar->audio_track || !ar->started || !data || size <= 0) {
        return 0;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return 0;
    }
    
    // Создаём byte array
    jbyteArray array = (*env)->NewByteArray(env, size);
    if (!array) {
        LOGE("Failed to allocate byte array");
        return 0;
    }
    
    // Копируем данные
    (*env)->SetByteArrayRegion(
        env, array, 0, size, (const jbyte *) data
    );
    
    // Записываем в AudioTrack
    jint written = (*env)->CallIntMethod(
        env, ar->audio_track, ar->write_mid,
        array, 0, size
    );
    
    (*env)->DeleteLocalRef(env, array);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("Exception in AudioTrack.write()");
        (*env)->ExceptionClear(env);
        return 0;
    }
    
    if (written < 0) {
        LOGE("AudioTrack.write() returned error: %d", written);
        return 0;
    }
    
    return written;
}

void audio_render_release(AudioRenderAndroid *ar) {
    if (!ar->audio_track) {
        return;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (env) {
        // Останавливаем и освобождаем
        (*env)->CallVoidMethod(env, ar->audio_track, ar->stop_mid);
        (*env)->CallVoidMethod(env, ar->audio_track, ar->release_mid);
        
        if ((*env)->ExceptionCheck(env)) {
            LOGE("Exception in AudioTrack release");
            (*env)->ExceptionClear(env);
        }
        
        (*env)->DeleteGlobalRef(env, ar->audio_track);
    }
    
    ar->audio_track = NULL;
    ar->started = false;
    
    LOGI("AudioTrack released");
}

int64_t audio_render_get_playback_head(AudioRenderAndroid *ar) {
    if (!ar->audio_track) {
        return 0;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return 0;
    }
    
    // Шаг 31.3: Получение текущего audio time через getPlaybackHeadPosition
    jclass at_cls = (*env)->GetObjectClass(env, ar->audio_track);
    if (!at_cls) {
        return 0;
    }
    
    jmethodID get_pos_mid = (*env)->GetMethodID(env, at_cls, "getPlaybackHeadPosition", "()I");
    if (!get_pos_mid) {
        (*env)->DeleteLocalRef(env, at_cls);
        return 0;
    }
    
    jint position = (*env)->CallIntMethod(env, ar->audio_track, get_pos_mid);
    
    (*env)->DeleteLocalRef(env, at_cls);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("Exception in AudioTrack.getPlaybackHeadPosition()");
        (*env)->ExceptionClear(env);
        return 0;
    }
    
    return (int64_t)position;
}

/// Получить audio latency (ШАГ 4)
int audio_render_get_latency(AudioRenderAndroid *ar) {
    if (!ar->audio_track) {
        return 0;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return 0;
    }
    
    // ШАГ 4: Получение latency через AudioTrack.getLatency()
    jclass at_cls = (*env)->GetObjectClass(env, ar->audio_track);
    if (!at_cls) {
        return 0;
    }
    
    jmethodID get_latency_mid = (*env)->GetMethodID(env, at_cls, "getLatency", "()I");
    if (!get_latency_mid) {
        (*env)->DeleteLocalRef(env, at_cls);
        return 0;
    }
    
    jint latency_ms = (*env)->CallIntMethod(env, ar->audio_track, get_latency_mid);
    
    (*env)->DeleteLocalRef(env, at_cls);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("Exception in AudioTrack.getLatency()");
        (*env)->ExceptionClear(env);
        return 0;
    }
    
    return (int)latency_ms;
}

void audio_render_flush(AudioRenderAndroid *ar) {
    if (!ar->audio_track) {
        return;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return;
    }
    
    // Шаг 31.8: Flush AudioTrack (только при seek)
    jclass at_cls = (*env)->GetObjectClass(env, ar->audio_track);
    if (!at_cls) {
        return;
    }
    
    jmethodID flush_mid = (*env)->GetMethodID(env, at_cls, "flush", "()V");
    if (flush_mid) {
        (*env)->CallVoidMethod(env, ar->audio_track, flush_mid);
        
        if ((*env)->ExceptionCheck(env)) {
            LOGE("Exception in AudioTrack.flush()");
            (*env)->ExceptionClear(env);
        }
    }
    
    (*env)->DeleteLocalRef(env, at_cls);
    
    LOGI("AudioTrack flushed");
}

/// Получить состояние воспроизведения AudioTrack
///
/// @param ar Аудиорендер
/// @return PLAYSTATE_STOPPED (1), PLAYSTATE_PAUSED (2), PLAYSTATE_PLAYING (3), или -1 при ошибке
int audio_render_get_play_state(AudioRenderAndroid *ar) {
    if (!ar || !ar->audio_track) {
        return -1;
    }
    
    JNIEnv *env = get_env(ar->jvm);
    if (!env) {
        return -1;
    }
    
    if (!ar->get_play_state_mid) {
        // Получаем method ID если ещё не получен
        jclass at_cls = (*env)->GetObjectClass(env, ar->audio_track);
        if (!at_cls) {
            return -1;
        }
        
        ar->get_play_state_mid = (*env)->GetMethodID(env, at_cls, "getPlayState", "()I");
        (*env)->DeleteLocalRef(env, at_cls);
        
        if (!ar->get_play_state_mid) {
            LOGE("Failed to get getPlayState method ID");
            return -1;
        }
    }
    
    jint state = (*env)->CallIntMethod(env, ar->audio_track, ar->get_play_state_mid);
    
    if ((*env)->ExceptionCheck(env)) {
        LOGE("Exception in AudioTrack.getPlayState()");
        (*env)->ExceptionClear(env);
        return -1;
    }
    
    return (int)state;
}

