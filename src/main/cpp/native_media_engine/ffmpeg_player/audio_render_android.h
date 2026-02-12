#pragma once

#include <jni.h>
#include <stdint.h>
#include <stdbool.h>

/// Нативный аудиорендер для Android (AudioTrack через JNI)
///
/// Прямой вывод PCM в AudioTrack без MediaCodec и ffmpeg-kit.
/// Используется в audio_render_thread для записи декодированных сэмплов.
typedef struct AudioRenderAndroid {
    /// JavaVM для JNI
    JavaVM *jvm;
    
    /// AudioTrack объект (global ref)
    jobject audio_track;
    
    /// Указатель на PlayerContext (для обновления audio_state)
    void *player_ctx;
    
    /// JNI method IDs
    jmethodID write_mid;   // write([BII)I
    jmethodID play_mid;   // play()V
    jmethodID pause_mid;  // pause()V
    jmethodID stop_mid;   // stop()V
    jmethodID release_mid; // release()V
    jmethodID get_play_state_mid; // getPlayState()I
    
    /// Параметры аудио
    int sample_rate;
    int channels;
    int bytes_per_sample; // 2 для PCM 16-bit
    
    /// Флаг, что AudioTrack запущен
    bool started;
} AudioRenderAndroid;

/// Инициализировать AudioTrack
///
/// @param ar Аудиорендер
/// @param jvm JavaVM для JNI
/// @param sample_rate Sample rate (Hz)
/// @param channels Количество каналов (1 = mono, 2 = stereo)
/// @return true при успехе, false при ошибке
bool audio_render_init(AudioRenderAndroid *ar,
                       JavaVM *jvm,
                       int sample_rate,
                       int channels);

/// Запустить воспроизведение
///
/// @param ar Аудиорендер
void audio_render_start(AudioRenderAndroid *ar);

/// Приостановить воспроизведение
///
/// @param ar Аудиорендер
void audio_render_pause(AudioRenderAndroid *ar);

/// Остановить воспроизведение
///
/// @param ar Аудиорендер
void audio_render_stop(AudioRenderAndroid *ar);

/// Освободить ресурсы AudioTrack
///
/// @param ar Аудиорендер
void audio_render_release(AudioRenderAndroid *ar);

/// Записать PCM данные в AudioTrack
///
/// @param ar Аудиорендер
/// @param data PCM данные (S16 interleaved)
/// @param size Размер данных в байтах
/// @return Количество записанных байт, или 0 при ошибке
int audio_render_write(AudioRenderAndroid *ar,
                       const uint8_t *data,
                       int size);

/// Используется для точного расчёта audio clock
/// @param ar Аудиорендер
/// @return Playback head position в сэмплах, или 0 при ошибке
int64_t audio_render_get_playback_head(AudioRenderAndroid *ar);

/// Получить latency AudioTrack
///
/// @param ar Аудиорендер
/// @return Latency в миллисекундах
int audio_render_get_latency(AudioRenderAndroid *ar);

/// Очистить буфер AudioTrack (Шаг 31.8 - Seek handling)
///
/// @param ar Аудиорендер
void audio_render_flush(AudioRenderAndroid *ar);

/// Получить состояние воспроизведения AudioTrack
///
/// @param ar Аудиорендер
/// @return PLAYSTATE_STOPPED (1), PLAYSTATE_PAUSED (2), PLAYSTATE_PLAYING (3), или -1 при ошибке
int audio_render_get_play_state(AudioRenderAndroid *ar);

