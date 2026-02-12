#include "video_sync.h"
#include <unistd.h>
#include <math.h>
#include <android/log.h>

#define LOG_TAG "VideoSync"
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

VideoSyncResult video_sync_and_wait(double video_pts, double audio_clock) {
    // Шаг 36.5: Sync logic (сердце шага)
    double diff = video_pts - audio_clock;
    
    // Шаг 36.7: Решение по кадру
    if (fabs(diff) < AV_SYNC_THRESHOLD_MIN) {
        // ✅ OK — render immediately (Шаг 36.7)
        return VIDEO_SYNC_RENDER;
    } else if (diff > 0) {
        // 🕒 video ahead → wait (Шаг 36.7)
        int delay_us = (int)(diff * 1e6);
        // Ограничиваем максимальный sleep (не более 50ms за раз)
        if (delay_us > 50000) {
            delay_us = 50000;
        }
        usleep(delay_us);
        ALOGD("SLEEP: video_pts=%.3f, audio=%.3f, diff=%.3f, sleep=%d us",
              video_pts, audio_clock, diff, delay_us);
        return VIDEO_SYNC_SLEEP;
    } else if (diff < -AV_SYNC_THRESHOLD_MAX) {
        // Видео отстало
        if (fabs(diff) > AV_SYNC_DROP_THRESHOLD) {
            // 🧨 video way behind → drop frame (Шаг 36.7)
            ALOGD("DROP: video_pts=%.3f, audio=%.3f, diff=%.3f", 
                  video_pts, audio_clock, diff);
            return VIDEO_SYNC_DROP;
        }
        // 🕒 slight delay → render anyway (Шаг 36.7)
        return VIDEO_SYNC_RENDER;
    }
    
    // Норма → render
    return VIDEO_SYNC_RENDER;
}

double video_sync_compute_delay(double video_pts, double audio_clock) {
    return video_pts - audio_clock;
}

