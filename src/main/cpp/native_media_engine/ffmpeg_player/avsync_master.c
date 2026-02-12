#include "avsync_master.h"
#include "ffmpeg_player.h"
#include "video_renderer.h"
#include "audio_renderer.h"
#include "avsync_gate.h"  // для avsync_gate_is_open
#include <math.h>
#include <android/log.h>
#include <sys/time.h>

#define LOG_TAG "AvSyncMaster"
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-MASTER - пороги для FATAL условий
#define CLOCK_STALL_THRESHOLD_MS 500      // 500ms - порог для clock stall
#define DRIFT_RUNAWAY_THRESHOLD 1.0       // 1 секунда - порог для drift runaway
#define DRIFT_RUNAWAY_FRAMES 30           // 30 кадров подряд с drift > 1s

// Глобальные переменные для отслеживания FATAL условий
static double g_last_master_clock = 0.0;
static int64_t g_last_master_clock_time = 0;  // миллисекунды
static double g_last_drift = 0.0;
static int g_drift_runaway_count = 0;

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-MASTER M1, M2, M3 - определение master clock
AvSyncMasterStatus avsync_master_determine(PlayerContext *ctx) {
    AvSyncMasterStatus status = {0};
    status.is_valid = false; // По умолчанию invalid
    
    if (!ctx) {
        status.type = AVSYNC_MASTER_NONE;
        return status;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: M3 - FATAL: hasAudio == true AND audioState != PLAYING AND videoState == PLAYING
    if (ctx->has_audio == 1 && 
        ctx->audio_state != AUDIO_PLAYING && 
        ctx->state.state == PLAYBACK_RUNNING && 
        !ctx->paused) {
        // ❌ INVALID STATE - это текущий баг
        ALOGE("❌ AVSYNC-MASTER FATAL: hasAudio=true, audioState=%d != PLAYING, videoState=PLAYING", 
              ctx->audio_state);
        status.type = AVSYNC_MASTER_NONE;
        status.is_valid = false;
        // Эмитим FATAL событие
        extern void native_player_emit_error_event(const char *message);
        native_player_emit_error_event("AUDIO_MASTER_LOST: hasAudio but audioState != PLAYING");
        return status;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: M1 - Если есть аудио → Audio MASTER
    if (ctx->has_audio == 1 && ctx->audio_state == AUDIO_PLAYING) {
        status.type = AVSYNC_MASTER_AUDIO;
        status.audio_clock_valid = true;
        
        // Получаем audio clock из AudioState
        extern double audio_get_clock(AudioState *as);
        if (ctx->audio && ctx->audio->clock.valid) {
            status.clock_value = audio_get_clock(ctx->audio);
        } else {
            status.clock_value = 0.0;
        }
        
        ALOGD("🎵 AVSYNC-MASTER: Audio MASTER (audio_clock=%.3f)", status.clock_value);
    }
    // 🔥 КРИТИЧЕСКИЙ FIX: M2 - Video-only → Video MASTER
    else if (ctx->has_audio == 0) {
        status.type = AVSYNC_MASTER_VIDEO;
        status.video_clock_valid = true;
        
        // Получаем video clock из VideoState
        extern double video_get_clock(VideoState *vs);
        if (ctx->video && ctx->video->clock.valid) {
            status.clock_value = video_get_clock(ctx->video);
            status.is_valid = true; // Video clock валиден
        } else {
            // Fallback: используем последний отрисованный PTS
            status.clock_value = ctx->video ? ctx->video->clock.pts_sec : 0.0;
            // 🔥 FIX 2: Video-only → разрешить "idle clock"
            // Для video-only режима до первого frame clock = IDLE (это нормально)
            // Не считаем это как invalid master
            status.is_valid = true; // ⬅️ КЛЮЧЕВО: video-only может иметь idle clock до первого frame
        }
        
        // 🔒 БОНУС: защитный ASSERT (оставь!)
        #ifdef DEBUG
        if (ctx->has_audio == 0 && status.type != AVSYNC_MASTER_VIDEO) {
            ALOGE("❌ AVSYNC-MASTER ASSERT FAILED: Video-only file cannot have non-video master (FATAL)");
            abort();
        }
        #endif
        
        ALOGD("🎞 AVSYNC-MASTER: Video MASTER (video_clock=%.3f, valid=%d)", 
              status.clock_value, status.is_valid);
    }
    // 🔥 КРИТИЧЕСКИЙ FIX: Paused или Seeking → NONE
    else {
        status.type = AVSYNC_MASTER_NONE;
        status.is_valid = false;
        ALOGD("⏸ AVSYNC-MASTER: NONE (paused or seeking)");
    }
    
    // Обновляем время последнего обновления
    struct timeval tv;
    gettimeofday(&tv, NULL);
    status.last_update_time = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
    
    return status;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: Проверить валидность master clock
bool avsync_master_is_valid(PlayerContext *ctx, const AvSyncMasterStatus *master_status) {
    if (!ctx || !master_status) {
        return false;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Audio MASTER валиден ТОЛЬКО если:
    // - audioState == PLAYING
    // - AudioTrack.playState == PLAYING (проверяется через clock.valid)
    // - noAudioException (проверяется через clock обновляется)
    if (master_status->type == AVSYNC_MASTER_AUDIO) {
        bool is_valid = (ctx->audio_state == AUDIO_PLAYING) &&
                       (ctx->audio != NULL) &&
                       (ctx->audio->clock.valid == 1);
        
        // Дополнительная проверка: audio clock должен быть > 0
        if (is_valid) {
            extern double audio_get_clock(AudioState *as);
            double audio_clock = audio_get_clock(ctx->audio);
            is_valid = !isnan(audio_clock) && audio_clock > 0.0;
        }
        
        return is_valid;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Video MASTER валиден ТОЛЬКО если:
    // - eglSwapBuffers был выполнен (проверяется через video_clock активен)
    // - frame реально показан (проверяется через video_clock обновляется)
    // - есть VSYNC timestamp (проверяется через clock_is_active)
    if (master_status->type == AVSYNC_MASTER_VIDEO) {
        bool is_valid = false;
        
        if (ctx->video) {
            is_valid = ctx->video->clock.valid;
            
            // Дополнительная проверка: video clock должен быть > 0
            if (is_valid) {
                extern double video_get_clock(VideoState *vs);
                double video_clock = video_get_clock(ctx->video);
                is_valid = (video_clock > 0.0) && !isnan(video_clock);
            }
        }
        
        return is_valid;
    }
    
    return false;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC GATE - проверка, разрешена ли AVSYNC операция
/// Используем функцию из avsync_gate.h для проверки gate
bool avsync_master_gate_is_open(PlayerContext *ctx) {
    if (!ctx) {
        return false;
    }
    
    // Используем функцию из avsync_gate.h
    return avsync_gate_is_open(&ctx->avsync_gate);
}

/// 🔥 КРИТИЧЕСКИЙ FIX: Проверить FATAL условия
bool avsync_check_fatal_conditions(PlayerContext *ctx, const AvSyncMasterStatus *master_status) {
    if (!ctx || !master_status) {
        return false;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: F1 - Audio master потерян
    // audioState == PLAYING → audio exception → audioState != PLAYING → video still playing
    if (ctx->has_audio == 1 && 
        ctx->audio_state != AUDIO_PLAYING && 
        ctx->audio_state != AUDIO_NO_AUDIO &&
        ctx->audio_state != AUDIO_INITIALIZING &&
        ctx->state.state == PLAYBACK_RUNNING && 
        !ctx->paused) {
        ALOGE("❌ AVSYNC-MASTER F1: Audio master lost (hasAudio=true, audioState=%d, videoState=PLAYING)", 
              ctx->audio_state);
        extern void native_player_emit_error_event(const char *message);
        native_player_emit_error_event("AUDIO_MASTER_LOST");
        return true;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: F2 - Clock stall
    // masterClock not advanced > 500ms
    if (master_status->is_valid && master_status->clock_value > 0.0) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t current_time_ms = (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
        
        if (g_last_master_clock_time > 0) {
            int64_t elapsed_ms = current_time_ms - g_last_master_clock_time;
            double clock_delta = master_status->clock_value - g_last_master_clock;
            
            // Если clock не изменился более чем на 500ms → stall
            if (elapsed_ms > CLOCK_STALL_THRESHOLD_MS && fabs(clock_delta) < 0.001) {
                ALOGE("❌ AVSYNC-MASTER F2: Clock stall (master_clock=%.3f, elapsed=%lld ms)", 
                      master_status->clock_value, (long long)elapsed_ms);
                extern void native_player_emit_error_event(const char *message);
                native_player_emit_error_event("CLOCK_STALL");
                return true;
            }
        }
        
        g_last_master_clock = master_status->clock_value;
        g_last_master_clock_time = current_time_ms;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: F3 - Drift runaway
    // drift > 1s for > N frames
    // (Это будет проверяться в video_sync_and_wait, здесь только логируем)
    
    return false;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: Получить текущее значение master clock
double avsync_master_get_clock(PlayerContext *ctx, const AvSyncMasterStatus *master_status) {
    if (!ctx || !master_status || !master_status->is_valid) {
        return 0.0;
    }
    
    return master_status->clock_value;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: Вычислить drift между audio и video
double avsync_compute_drift(PlayerContext *ctx, const AvSyncMasterStatus *master_status, double video_pts) {
    if (!ctx || !master_status || !master_status->is_valid) {
        return 0.0;
    }
    
    double master_clock = avsync_master_get_clock(ctx, master_status);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Если Audio MASTER: drift = video_pts - audio_pts
    if (master_status->type == AVSYNC_MASTER_AUDIO) {
        double drift = video_pts - master_clock;
        
        // 🔥 КРИТИЧЕСКИЙ FIX: F3 - Drift runaway detection
        if (fabs(drift) > DRIFT_RUNAWAY_THRESHOLD) {
            g_drift_runaway_count++;
            if (g_drift_runaway_count >= DRIFT_RUNAWAY_FRAMES) {
                ALOGE("❌ AVSYNC-MASTER F3: Drift runaway (drift=%.3f, count=%d)", 
                      drift, g_drift_runaway_count);
                extern void native_player_emit_error_event(const char *message);
                native_player_emit_error_event("DRIFT_RUNAWAY");
                g_drift_runaway_count = 0;  // Сбрасываем счётчик после FATAL
            }
        } else {
            g_drift_runaway_count = 0;  // Сбрасываем счётчик при нормальном drift
        }
        
        g_last_drift = drift;
        return drift;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Если Video MASTER: drift = audio_pts - video_pts
    if (master_status->type == AVSYNC_MASTER_VIDEO) {
        // Получаем audio clock из AudioState
        extern double audio_get_clock(AudioState *as);
        double audio_pts = ctx->audio ? audio_get_clock(ctx->audio) : 0.0;
        double drift = audio_pts - master_clock;
        
        // Аналогичная проверка drift runaway для video master
        if (fabs(drift) > DRIFT_RUNAWAY_THRESHOLD) {
            g_drift_runaway_count++;
            if (g_drift_runaway_count >= DRIFT_RUNAWAY_FRAMES) {
                ALOGE("❌ AVSYNC-MASTER F3: Drift runaway (video master, drift=%.3f, count=%d)", 
                      drift, g_drift_runaway_count);
                extern void native_player_emit_error_event(const char *message);
                native_player_emit_error_event("DRIFT_RUNAWAY");
                g_drift_runaway_count = 0;
            }
        } else {
            g_drift_runaway_count = 0;
        }
        
        g_last_drift = drift;
        return drift;
    }
    
    return 0.0;
}

