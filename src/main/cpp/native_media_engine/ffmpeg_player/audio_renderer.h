#ifndef AUDIO_RENDERER_H
#define AUDIO_RENDERER_H

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswresample/swresample.h"
#include "packet_queue.h"
#include "frame_queue.h"
#include "audio_render_android.h"
#include "clock.h"  // для Clock

// === 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16 ===

/// Audio clock (PTS-based, канонический)
///
/// 🔒 AUDIO CLOCK = PTS последнего реально отправленного в AudioTrack фрейма
/// ❌ НЕ использует AudioTrack.getPlaybackHeadPosition() (залипает на Huawei/HiSilicon)
/// ❌ НЕ использует system time
/// ❌ НЕ использует estimated latency
///
/// Каноническое определение:
///   audio_clock = last_audio_frame_pts + last_audio_frame_duration - audio_latency_compensation
///
/// Инварианты:
///   - ✅ Audio clock = PTS последнего сыгранного сэмпла + duration - latency
///   - ✅ Clock обновляется ТОЛЬКО после AudioTrack.write()
///   - ✅ Если last_update_us > 500ms → stalled
///   - ✅ Extrapolation только если playing
typedef struct {
    double clock;              // 🔥 основной audio clock (seconds)
    double last_pts;           // PTS последнего аудиофрейма (seconds)
    double last_duration;      // длительность последнего фрейма (seconds)
    double latency;            // задержка AudioTrack (seconds)
    int64_t last_update_us;    // monotonic time (microseconds) последнего обновления
    int valid;                 // Флаг валидности clock (1 = valid, 0 = invalid)
} AudioClock;

/// Состояние аудио декодера и рендерера
///
/// Управляет:
/// - Декодированием аудио пакетов
/// - Ресемплингом (swr)
/// - Audio clock (master clock для всей системы)
/// - Потоком декодирования и рендеринга
typedef struct {
    /// Codec context для аудио
    AVCodecContext *codecCtx;
    
    /// Очередь пакетов (из demux thread)
    PacketQueue *packetQueue;
    
    /// Очередь декодированных кадров (в audio render thread)
    FrameQueue *frameQueue;
    
    /// SwrContext для ресемплинга
    SwrContext *swr;
    
    /// Буфер для ресемплинга (PCM данные)
    uint8_t *out_buf;
    
    /// Размер буфера для ресемплинга
    int out_buf_size;
    
    /// Audio clock (master clock - главные часы системы) - DEPRECATED
    /// 🎯 MASTER CLOCK - видео подстраивается под аудио
    /// Обновляется ТОЛЬКО в audio render thread на основе samples_written
    /// ❌ DEPRECATED: используйте clock.clock
    double audio_clock;  // DEPRECATED: используйте clock.clock
    
    /// PTS начала воспроизведения (для расчёта audio_clock) - DEPRECATED
    /// ❌ DEPRECATED: используйте clock.last_pts
    double audio_pts_start;  // DEPRECATED
    
    /// Общее количество сэмплов, записанных в AudioTrack - DEPRECATED
    /// ❌ DEPRECATED: не используется для clock
    int64_t samples_written;  // DEPRECATED
    
    /// Playback head position из AudioTrack (для точного clock) - DEPRECATED
    /// ❌ DEPRECATED: не использовать как clock source
    int64_t playback_head_samples;  // DEPRECATED: не использовать как clock source
    
    /// Sample rate (Hz)
    int sample_rate;
    
    /// Количество каналов
    int channels;
    
    /// Формат сэмплов (всегда AV_SAMPLE_FMT_S16 для AudioTrack)
    enum AVSampleFormat sample_fmt;
    
    /// Флаг прерывания потока
    int abort;
    
    /// Флаг паузы (для pause/resume)
    int paused;
    
    /// Указатель на PlayerContext (для установки audio_finished при EOF)
    void *player_ctx;
    
    /// Thread для декодирования
    pthread_t decodeThread;
    
    /// Флаг, что decode thread был запущен
    int decodeThread_started;
    
    /// Флаг, что decode thread был join'нут
    int decodeThread_joined;
    
    /// Thread для рендеринга (AudioTrack)
    pthread_t renderThread;
    
    /// Флаг, что render thread был запущен
    int renderThread_started;
    
    /// Флаг, что render thread был join'нут
    int renderThread_joined;
    
    /// Флаг, что audio_threads_stop() уже был вызван (защита от повторного вызова)
    int threads_stopped;
    
    /// Нативный аудиорендер (AudioTrack через JNI)
    AudioRenderAndroid audio_render;
    
    /// JavaVM для JNI (для audio_render)
    JavaVM *jvm;
    
    // === Audio Latency & Drift Correction (ШАГ 5) ===
    
    /// Усреднённый дрейф аудио (для коррекции)
    double audio_diff_avg;
    
    /// Накопительная сумма diff для усреднения (ШАГ 5.3)
    double audio_diff_cum;
    
    /// Счётчик для усреднения (ШАГ 5.3)
    int audio_diff_count;
    
    /// Ring buffer для усреднения дрейфа (экспоненциальное)
    double audio_diff_avg_coef;
    
    /// Порог дрейфа для коррекции (40 ms, ШАГ 5.4)
    double audio_diff_threshold;
    
    /// Порог "не нужна коррекция" (100 ms, ШАГ 5.4)
    double audio_no_sync_threshold;
    
    /// Количество сэмплов для коррекции (wanted_nb_samples)
    int wanted_nb_samples;
    
    /// Целевой sample rate для коррекции (ШАГ 5.5)
    double target_sample_rate;
    
    /// Latency AudioTrack (в миллисекундах)
    int audio_latency_ms;
    
    // === 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC IMPLEMENTATION - ШАГ 4 ===
    
    /// 🔥 ЕДИНСТВЕННЫЙ ИСТОЧНИК AUDIO CLOCK
    /// AudioClock структура (REAL TIME из AudioTrack.getPlaybackHeadPosition())
    /// Обновляется ТОЛЬКО через audio_clock_update() в audio render thread
    AudioClock clock;  // 🔥 ЕДИНСТВЕННЫЙ ИСТОЧНИК AUDIO CLOCK
    
    // Legacy поля (deprecated, для обратной совместимости)
    double audio_clock_pts;  // DEPRECATED: используйте clock.clock_sec
    double last_audio_clock_pts;  // DEPRECATED
    double clock_base_pts;  // DEPRECATED
    double clock_base_time_sec;  // DEPRECATED
    int clock_valid;  // DEPRECATED: используйте clock.valid
    int track_failed;  // DEPRECATED: используйте clock.stalled
} AudioState;

/// Инициализировать аудио декодер
///
/// @param as Состояние аудио
/// @param stream Аудио стрим из AVFormatContext
/// @return 0 при успехе, <0 при ошибке
int audio_decoder_init(AudioState *as, AVStream *stream);

/// Инициализировать SwrContext для ресемплинга
///
/// Всегда используем swr, даже если формат "совпадает"
/// (иначе сломается на другом устройстве)
///
/// @param as Состояние аудио
/// @return 0 при успехе, <0 при ошибке
int audio_swr_init(AudioState *as);

/// Запустить поток декодирования аудио
///
/// @param as Состояние аудио
/// @return 0 при успехе, <0 при ошибке
int audio_decode_thread_start(AudioState *as);

/// Остановить поток декодирования аудио
///
/// @param as Состояние аудио
void audio_decode_thread_stop(AudioState *as);

/// Освободить ресурсы аудио декодера
///
/// @param as Состояние аудио
void audio_decoder_destroy(AudioState *as);

/// Получить текущий audio clock
///
/// @param as Состояние аудио
/// @return Текущий audio clock в секундах
double audio_get_clock(AudioState *as);

/// Запустить потоки декодирования и рендеринга аудио
///
/// @param as Состояние аудио
/// @param jvm JavaVM для JNI (для audio_render)
/// @return 0 при успехе, <0 при ошибке
int audio_threads_start(AudioState *as, JavaVM *jvm);

/// Остановить потоки декодирования и рендеринга аудио
///
/// @param as Состояние аудио
void audio_threads_stop(AudioState *as);

/// Сбросить audio clock (для seek) - Legacy функция для обратной совместимости (deprecated)
///
/// @param as Состояние аудио
/// @param seek_pos Новая позиция в секундах
/// @deprecated Используйте audio_clock_reset(AudioClock *c)
void audio_clock_reset_legacy(AudioState *as, double seek_pos);

/// Проверить, пуста ли очередь аудио (для underrun detection)
///
/// @param as Состояние аудио
/// @return true если очередь пуста
bool audio_queue_empty(AudioState *as);

/// Инициализировать drift correction
///
/// @param as Состояние аудио
void audio_drift_correction_init(AudioState *as);

/// Применить drift correction перед выводом
///
/// Корректирует количество сэмплов для компенсации дрейфа
///
/// @param as Состояние аудио
/// @param nb_samples Исходное количество сэмплов
/// @return Скорректированное количество сэмплов
int audio_drift_correction_apply(AudioState *as, int nb_samples);

/// Обновить усреднённый дрейф
///
/// @param as Состояние аудио
/// @param drift Текущий дрейф (audio_clock - master_clock)
void audio_drift_correction_update(AudioState *as, double drift);

/// Сбросить drift correction (при seek/resume)
///
/// @param as Состояние аудио
void audio_drift_correction_reset(AudioState *as);

/// Получить latency AudioTrack (через JNI)
///
/// @param as Состояние аудио
/// @param env JNI environment
/// @return Latency в миллисекундах
int audio_get_latency(AudioState *as, JNIEnv *env);

/// Приостановить воспроизведение аудио
///
/// @param as Состояние аудио
void audio_pause(AudioState *as);

/// Возобновить воспроизведение аудио
///
/// @param as Состояние аудио
void audio_resume(AudioState *as);

// === 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 7 ===

// ❌ УДАЛЕНО: audio_clock_update() который использовал getPlaybackHeadPosition()
// Clock обновляется ТОЛЬКО в audio_render_thread после AudioTrack.write() с PTS фрейма

/// Проверить audio stall (Huawei / HiSilicon case)
///
/// @param as Состояние аудио
void audio_check_stall(AudioState *as);

/// Попытаться восстановить AudioTrack после stall (one-shot recovery)
///
/// @param as Состояние аудио
void audio_try_recover(AudioState *as);

/// Проверить audio stall (Huawei / HiSilicon case)
///
/// @param c AudioClock
/// @return 1 если stalled, 0 если running
int audio_clock_is_stalled(AudioClock *c);

/// Проверить ASSERT-ы для audio clock (обязательные)
///
/// @param as Состояние аудио
/// @param ctx PlayerContext
void audio_clock_assert(AudioState *as, void *ctx);

/// Инициализировать AudioClock
///
/// @param c AudioClock структура
void audio_clock_init(AudioClock *c);

/// Сбросить AudioClock (при seek)
///
/// @param c AudioClock структура
void audio_clock_reset(AudioClock *c);

#endif // AUDIO_RENDERER_H

