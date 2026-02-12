/// 🔴 ЗАДАЧА 5: Error handling для Native FFmpeg Player
///
/// Обработка ошибок:
/// - Фиксация ошибки в PlayerContext
/// - Уведомление Flutter через JNI
/// - Остановка плеера при фатальных ошибках

#include "ffmpeg_player_error.h"
#include "ffmpeg_player_lifecycle.h"  // Для render_loop_stop
#include "audio_renderer.h"
#include "clock.h"
#include <pthread.h>
#include <string.h>
#include <android/log.h>

#define LOG_TAG "FFmpegPlayerError"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Установить ошибку в PlayerContext (atomic + single-shot)
void player_set_error(PlayerContext *ctx, PlayerError err) {
    if (!ctx) {
        return;
    }
    
    pthread_mutex_lock(&ctx->error_mutex);
    
    // Первая ошибка - главная, остальные игнорируются
    if (ctx->error == PLAYER_ERROR_NONE) {
        ctx->error = (int)err;
        ctx->error_reported = 0;
        ALOGE("❌ player_set_error: Error set: %d", err);
    } else {
        ALOGD("player_set_error: Error already set (%d), ignoring new error (%d)", ctx->error, err);
    }
    
    pthread_mutex_unlock(&ctx->error_mutex);
}

/// Получить ошибку из PlayerContext
PlayerError player_get_error(PlayerContext *ctx) {
    if (!ctx) {
        return PLAYER_ERROR_INTERNAL;
    }
    
    pthread_mutex_lock(&ctx->error_mutex);
    PlayerError err = (PlayerError)ctx->error;
    pthread_mutex_unlock(&ctx->error_mutex);
    
    return err;
}

/// Обработать фатальную ошибку
void player_handle_fatal_error(PlayerContext *ctx, PlayerError err) {
    if (!ctx) {
        return;
    }
    
    ALOGE("❌ player_handle_fatal_error: Fatal error %d", err);
    
    // Устанавливаем ошибку
    player_set_error(ctx, err);
    
    // Останавливаем render loop
    render_loop_stop(ctx);
    
    // Останавливаем audio
    if (ctx->audio) {
        clock_pause(&ctx->audio->clock, 1);
        ctx->audio->paused = 1;
    }
    
    ALOGI("✅ player_handle_fatal_error: Player stopped due to fatal error");
}

