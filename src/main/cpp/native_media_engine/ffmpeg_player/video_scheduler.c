/// ШАГ 1-4: VSync-driven video scheduler с frame interpolation
///
/// ШАГ 1: VSync-aligned frame scheduling через AChoreographer
/// ШАГ 2: Frame interpolation (linear) в shader
/// ШАГ 3: Audio clock = master clock
/// ШАГ 4: Точный audio clock с учётом latency AudioTrack

#include "video_scheduler.h"
#include "clock.h"
#include "frame_queue.h"
#include <android/log.h>
#include <android/choreographer.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#define LOG_TAG "VideoScheduler"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// ШАГ 1: Пороги для frame scheduling
#define FRAME_LATE_THRESHOLD   0.050   // 50 ms → drop
#define VSYNC_INTERVAL         0.016   // ~60 Hz (16.67 ms)

/// Получить текущее время (monotonic clock, секунды)
static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/// ШАГ 3, 4: Получить master clock (audio clock с учётом latency)
static double get_master_clock(VideoScheduler *scheduler) {
    if (!scheduler || !scheduler->audio_state) {
        // Fallback: system time
        return now_sec();
    }
    
    AudioState *as = scheduler->audio_state;
    
    // ШАГ 4: Получаем точный audio clock с учётом latency
    if (clock_is_active(&as->clock)) {
        // ШАГ 4: Используем clock_get_time, который учитывает latency
        return clock_get_time(&as->clock);
    }
    
    // Fallback: system time
    return now_sec();
}

/// ШАГ 1: VSync callback (вызывается на каждом VSync)
static void vsync_callback(long frameTimeNanos, void *userData) {
    VideoScheduler *scheduler = (VideoScheduler *)userData;
    
    if (!scheduler || scheduler->abort) {
        return;
    }
    
    VideoState *vs = scheduler->video_state;
    VideoRenderGL *vr = scheduler->video_render;
    
    if (!vs || !vr || !vs->frameQueue) {
        goto repost;
    }
    
    // ШАГ 1: Конвертируем VSync время в секунды
    double vsync_time = frameTimeNanos / 1e9;
    
    // ШАГ 3, 4: Получаем master clock (audio clock)
    double master_clock = get_master_clock(scheduler);
    
    // ШАГ 1: Peek текущий кадр (без извлечения)
    Frame *f0 = frame_queue_peek_ptr(vs->frameQueue);
    if (!f0 || !f0->frame) {
        goto repost;
    }
    
    // ШАГ 1: Получаем PTS кадра в секундах
    double frame_pts = f0->pts;
    if (isnan(frame_pts)) {
        goto repost;
    }
    
    // ШАГ 1: Проверяем, готов ли кадр к показу
    double diff = frame_pts - master_clock;
    
    if (diff < -FRAME_LATE_THRESHOLD) {
        // Кадр сильно опоздал → drop
        ALOGD("Frame too late: pts=%.3f clock=%.3f diff=%.3f (drop)", 
              frame_pts, master_clock, diff);
        frame_queue_next(vs->frameQueue);
        goto repost;
    }
    
    // ШАГ 2: Frame interpolation (linear)
    Frame *f1 = frame_queue_peek_next_ptr(vs->frameQueue);
    double alpha = 0.0;
    AVFrame *frame1_ptr = NULL;
    
    if (f1 && f1->frame && !isnan(f1->pts) && f1->pts > frame_pts) {
        double gap = f1->pts - frame_pts;
        
        // ШАГ 2: Interpolation только если gap > VSync interval
        if (gap > VSYNC_INTERVAL) {
            // ШАГ 2: Расчёт alpha для interpolation
            alpha = (master_clock - frame_pts) / gap;
            
            // Clamp alpha
            if (alpha < 0.0) alpha = 0.0;
            if (alpha >= 1.0) {
                // Переходим к следующему кадру
                frame_queue_next(vs->frameQueue);
                alpha = 0.0;
                frame1_ptr = NULL;
            } else {
                frame1_ptr = f1->frame;
            }
        }
    }
    
    // ШАГ 1, 2: Рендерим кадр (с interpolation если нужно)
    if (diff <= 0.0 || alpha > 0.0) {
        // Время показывать кадр
        int ret = video_render_gl_draw(vr, f0->frame, frame1_ptr, alpha);
        
        if (ret == 0) {
            ALOGD("Frame rendered: pts=%.3f clock=%.3f alpha=%.3f", 
                  frame_pts, master_clock, alpha);
            
            // Продвигаем очередь, если alpha >= 1.0 или кадр полностью показан
            if (alpha >= 1.0 || diff <= -VSYNC_INTERVAL) {
                frame_queue_next(vs->frameQueue);
            }
        } else {
            ALOGE("Error rendering frame, dropping. PTS: %.3f", frame_pts);
            frame_queue_next(vs->frameQueue);
        }
    }
    
repost:
    // ШАГ 1: Регистрируем следующий VSync callback
    if (!scheduler->abort && scheduler->active) {
        AChoreographer_postFrameCallback(
            (AChoreographer *)scheduler->choreographer,
            vsync_callback,
            scheduler
        );
    }
}

/// ШАГ 1: VSync thread (получает AChoreographer)
static void *vsync_thread(void *arg) {
    VideoScheduler *scheduler = (VideoScheduler *)arg;
    
    ALOGI("VSync thread started");
    
    // ШАГ 1: Получаем AChoreographer для текущего thread
    AChoreographer *choreographer = AChoreographer_getInstance();
    if (!choreographer) {
        ALOGE("Failed to get AChoreographer instance");
        return NULL;
    }
    
    scheduler->choreographer = choreographer;
    
    // ШАГ 1: Регистрируем первый VSync callback
    AChoreographer_postFrameCallback(
        choreographer,
        vsync_callback,
        scheduler
    );
    
    // Ждём завершения
    while (!scheduler->abort) {
        usleep(100000); // 100ms
    }
    
    ALOGI("VSync thread stopped");
    return NULL;
}

int video_scheduler_init(VideoScheduler *scheduler,
                         VideoState *video_state,
                         AudioState *audio_state,
                         VideoRenderGL *video_render) {
    if (!scheduler || !video_state || !video_render) {
        return -1;
    }
    
    memset(scheduler, 0, sizeof(VideoScheduler));
    
    scheduler->video_state = video_state;
    scheduler->audio_state = audio_state;
    scheduler->video_render = video_render;
    scheduler->active = false;
    scheduler->abort = false;
    
    pthread_mutex_init(&scheduler->mutex, NULL);
    
    ALOGI("Video scheduler initialized");
    return 0;
}

int video_scheduler_start(VideoScheduler *scheduler) {
    if (!scheduler) {
        return -1;
    }
    
    pthread_mutex_lock(&scheduler->mutex);
    
    if (scheduler->active) {
        pthread_mutex_unlock(&scheduler->mutex);
        return 0; // Уже запущен
    }
    
    scheduler->active = true;
    scheduler->abort = false;
    
    // ШАГ 1: Запускаем VSync thread
    int ret = pthread_create(&scheduler->vsync_thread, NULL, vsync_thread, scheduler);
    if (ret != 0) {
        ALOGE("Failed to create VSync thread: %d", ret);
        scheduler->active = false;
        pthread_mutex_unlock(&scheduler->mutex);
        return -1;
    }
    
    pthread_mutex_unlock(&scheduler->mutex);
    
    ALOGI("Video scheduler started");
    return 0;
}

void video_scheduler_stop(VideoScheduler *scheduler) {
    if (!scheduler) {
        return;
    }
    
    pthread_mutex_lock(&scheduler->mutex);
    
    if (!scheduler->active) {
        pthread_mutex_unlock(&scheduler->mutex);
        return;
    }
    
    scheduler->abort = true;
    scheduler->active = false;
    
    pthread_mutex_unlock(&scheduler->mutex);
    
    // Ждём завершения VSync thread
    if (scheduler->vsync_thread) {
        pthread_join(scheduler->vsync_thread, NULL);
        scheduler->vsync_thread = 0;
    }
    
    scheduler->choreographer = NULL;
    
    ALOGI("Video scheduler stopped");
}

void video_scheduler_release(VideoScheduler *scheduler) {
    if (!scheduler) {
        return;
    }
    
    video_scheduler_stop(scheduler);
    pthread_mutex_destroy(&scheduler->mutex);
    
    ALOGI("Video scheduler released");
}

double video_scheduler_get_master_clock(VideoScheduler *scheduler) {
    return get_master_clock(scheduler);
}

