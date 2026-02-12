/// Шаг 30: Unified Playback State Machine

#include "playback_state_machine.h"
#include "ffmpeg_player.h"
#include "video_backend_mediacodec.h"
#include "video_render_gl.h"
#include "audio_renderer.h"
#include <android/log.h>
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "PlaybackState"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Проверить допустимость перехода (Шаг 30.4)
bool playback_can_transition(PlaybackStateMachineState from, PlaybackStateMachineState to) {
    // Из ERROR нельзя перейти никуда
    if (from == STATE_ERROR) {
        return false;
    }
    
    // Из IDLE нельзя сразу в PLAYING
    if (from == STATE_IDLE && to == STATE_PLAYING) {
        return false;
    }
    
    // Все остальные переходы допустимы
    return true;
}

void playback_context_init(PlaybackContext *ctx) {
    if (!ctx) {
        return;
    }
    
    memset(ctx, 0, sizeof(PlaybackContext));
    ctx->state = STATE_IDLE;
    ctx->use_hw_video = false;
    ctx->hw_failed = false;
    ctx->paused = false;
    ctx->abort_request = false;
    pthread_mutex_init(&ctx->mutex, NULL);
    
    ALOGI("Playback context initialized");
}

void playback_context_destroy(PlaybackContext *ctx) {
    if (!ctx) {
        return;
    }
    
    playback_stop(ctx);
    
    pthread_mutex_destroy(&ctx->mutex);
    memset(ctx, 0, sizeof(PlaybackContext));
    
    ALOGI("Playback context destroyed");
}

bool playback_set_state(PlaybackContext *ctx, PlaybackStateMachineState next_state) {
    if (!ctx) {
        return false;
    }
    
    pthread_mutex_lock(&ctx->mutex);
    
    PlaybackStateMachineState current_state = ctx->state;
    
    // Проверяем допустимость перехода (Шаг 30.4)
    if (!playback_can_transition(current_state, next_state)) {
        ALOGE("Invalid state transition: %d -> %d", current_state, next_state);
        pthread_mutex_unlock(&ctx->mutex);
        return false;
    }
    
    ctx->state = next_state;
    
    pthread_mutex_unlock(&ctx->mutex);
    
    // Уведомляем Flutter (Шаг 30.5)
    playback_notify_flutter_state(ctx, next_state);
    
    ALOGD("State transition: %d -> %d", current_state, next_state);
    
    return true;
}

PlaybackStateMachineState playback_get_state(PlaybackContext *ctx) {
    if (!ctx) {
        return STATE_ERROR;
    }
    
    pthread_mutex_lock(&ctx->mutex);
    PlaybackStateMachineState state = ctx->state;
    pthread_mutex_unlock(&ctx->mutex);
    
    return state;
}

int playback_init(PlaybackContext *ctx, bool use_hw) {
    if (!ctx) {
        return -1;
    }
    
    if (!playback_set_state(ctx, STATE_PREPARING)) {
        return -1;
    }
    
    ctx->use_hw_video = use_hw && !ctx->hw_failed;
    
    // TODO: Инициализация video/audio backends
    // Это будет сделано в ffmpeg_player.c
    
    if (!playback_set_state(ctx, STATE_READY)) {
        return -1;
    }
    
    ALOGI("Playback initialized (HW: %s)", ctx->use_hw_video ? "ON" : "OFF");
    
    return 0;
}

int playback_play(PlaybackContext *ctx) {
    if (!ctx) {
        return -1;
    }
    
    PlaybackStateMachineState current = playback_get_state(ctx);
    
    if (current == STATE_PLAYING) {
        return 0; // Уже играет
    }
    
    if (current != STATE_READY && current != STATE_PAUSED) {
        ALOGE("Cannot play from state: %d", current);
        return -1;
    }
    
    if (!playback_set_state(ctx, STATE_PLAYING)) {
        return -1;
    }
    
    ctx->paused = false;
    
    // TODO: Запуск video/audio backends
    // Это будет сделано в ffmpeg_player.c
    
    ALOGI("Playback started");
    
    return 0;
}

void playback_pause(PlaybackContext *ctx) {
    if (!ctx) {
        return;
    }
    
    PlaybackStateMachineState current = playback_get_state(ctx);
    
    if (current != STATE_PLAYING) {
        return;
    }
    
    if (!playback_set_state(ctx, STATE_PAUSED)) {
        return;
    }
    
    ctx->paused = true;
    
    // Шаг 30.7: Pause без пересоздания backends
    if (ctx->video_backend) {
        if (ctx->use_hw_video) {
            video_backend_mediacodec_pause((VideoBackendMediaCodec *)ctx->video_backend);
        }
        // TODO: pause для SW backend
    }
    
    if (ctx->audio_state) {
        audio_pause((AudioState *)ctx->audio_state);
    }
    
    ALOGI("Playback paused");
}

void playback_resume(PlaybackContext *ctx) {
    if (!ctx) {
        return;
    }
    
    PlaybackStateMachineState current = playback_get_state(ctx);
    
    if (current != STATE_PAUSED) {
        return;
    }
    
    if (!playback_set_state(ctx, STATE_PLAYING)) {
        return;
    }
    
    ctx->paused = false;
    
    // Resume backends
    if (ctx->video_backend) {
        if (ctx->use_hw_video) {
            video_backend_mediacodec_resume((VideoBackendMediaCodec *)ctx->video_backend);
        }
        // TODO: resume для SW backend
    }
    
    if (ctx->audio_state) {
        audio_resume((AudioState *)ctx->audio_state);
    }
    
    ALOGI("Playback resumed");
}

int playback_seek(PlaybackContext *ctx, double pts) {
    if (!ctx) {
        return -1;
    }
    
    PlaybackStateMachineState current = playback_get_state(ctx);
    
    if (current == STATE_IDLE || current == STATE_ERROR) {
        return -1;
    }
    
    // Шаг 30.8: Seek flow
    if (!playback_set_state(ctx, STATE_SEEKING)) {
        return -1;
    }
    
    // Flush backends
    if (ctx->video_backend) {
        if (ctx->use_hw_video) {
            video_backend_mediacodec_seek((VideoBackendMediaCodec *)ctx->video_backend, pts);
        } else {
            // TODO: seek для SW backend
        }
    }
    
    if (ctx->audio_state) {
        extern void audio_clock_reset(AudioClock *c);
        if (ctx->audio_state) {
            AudioState *as = (AudioState *)ctx->audio_state;
            audio_clock_reset(&as->clock);
        }
    }
    
    // Переходим в BUFFERING
    if (!playback_set_state(ctx, STATE_BUFFERING)) {
        return -1;
    }
    
    // После получения первого кадра перейдём в PLAYING
    // Это будет сделано в video_render_thread
    
    ALOGI("Playback seeked to %.3f", pts);
    
    return 0;
}

void playback_stop(PlaybackContext *ctx) {
    if (!ctx) {
        return;
    }
    
    ctx->abort_request = true;
    
    // Останавливаем backends
    if (ctx->video_backend) {
        if (ctx->use_hw_video) {
            video_backend_mediacodec_stop((VideoBackendMediaCodec *)ctx->video_backend);
        } else {
            // TODO: stop для SW backend
        }
    }
    
    if (ctx->audio_state) {
        audio_threads_stop((AudioState *)ctx->audio_state);
    }
    
    playback_set_state(ctx, STATE_IDLE);
    
    ALOGI("Playback stopped");
}

int playback_fallback_to_software(PlaybackContext *ctx) {
    if (!ctx || ctx->hw_failed) {
        return -1;
    }
    
    ALOGI("Falling back to software decode");
    
    // Шаг 30.9: HW → SW fallback
    ctx->hw_failed = true;
    ctx->use_hw_video = false;
    
    // Останавливаем HW backend
    if (ctx->video_backend && ctx->use_hw_video) {
        video_backend_mediacodec_stop((VideoBackendMediaCodec *)ctx->video_backend);
        video_backend_mediacodec_release((VideoBackendMediaCodec *)ctx->video_backend);
        ctx->video_backend = NULL;
    }
    
    // TODO: Инициализировать SW backend
    // Это будет сделано в ffmpeg_player.c
    
    ALOGI("Fallback to software completed");
    
    return 0;
}

void playback_handle_eof(PlaybackContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Шаг 30.10: EOF ≠ Error
    if (playback_get_state(ctx) != STATE_EOF) {
        playback_set_state(ctx, STATE_EOF);
        ALOGI("Playback reached EOF");
    }
}

void playback_update_position(PlaybackContext *ctx, double position) {
    if (!ctx) {
        return;
    }
    
    pthread_mutex_lock(&ctx->mutex);
    ctx->position = position;
    pthread_mutex_unlock(&ctx->mutex);
}

double playback_get_position(PlaybackContext *ctx) {
    if (!ctx) {
        return 0.0;
    }
    
    pthread_mutex_lock(&ctx->mutex);
    double position = ctx->position;
    pthread_mutex_unlock(&ctx->mutex);
    
    return position;
}

double playback_get_duration(PlaybackContext *ctx) {
    if (!ctx) {
        return 0.0;
    }
    
    return ctx->duration;
}

void playback_notify_flutter_state(PlaybackContext *ctx, PlaybackStateMachineState state) {
    if (!ctx || !ctx->player_ctx) {
        return;
    }
    
    // TODO: Уведомить Flutter через JNI
    // Это будет сделано в ffmpeg_player.c через notify_flutter_event
    
    const char *state_name = NULL;
    switch (state) {
        case STATE_IDLE: state_name = "idle"; break;
        case STATE_PREPARING: state_name = "preparing"; break;
        case STATE_READY: state_name = "ready"; break;
        case STATE_PLAYING: state_name = "playing"; break;
        case STATE_PAUSED: state_name = "paused"; break;
        case STATE_SEEKING: state_name = "seeking"; break;
        case STATE_BUFFERING: state_name = "buffering"; break;
        case STATE_EOF: state_name = "eof"; break;
        case STATE_ERROR: state_name = "error"; break;
    }
    
    if (state_name) {
        ALOGD("Notifying Flutter: state=%s", state_name);
        // notify_flutter_event(ctx->player_ctx, state_name);
    }
}

