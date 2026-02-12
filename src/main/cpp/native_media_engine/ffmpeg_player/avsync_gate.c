#include "avsync_gate.h"
#include <string.h>
#include <sys/time.h>
#include <android/log.h>

#define LOG_TAG "AVSyncGate"
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/// Получить текущее время в микросекундах
static int64_t get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

/// Инициализировать AVSyncGate
void avsync_gate_init(AVSyncGate *gate) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->master, AVSYNC_MASTER_NONE_GATE);
    atomic_store(&gate->master_valid, false);
    atomic_store(&gate->audio_clock_us, 0);
    atomic_store(&gate->video_clock_us, 0);
    atomic_store(&gate->last_master_advance_us, 0);
    atomic_store(&gate->seek_in_progress, false);
    
    memset(gate->invalidation_reason, 0, sizeof(gate->invalidation_reason));
    
    ALOGD("✅ AVSyncGate initialized");
}

/// Инвалидировать AVSYNC gate
void avsync_gate_invalidate(AVSyncGate *gate, const char *reason) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->master_valid, false);
    
    if (reason) {
        strncpy(gate->invalidation_reason, reason, sizeof(gate->invalidation_reason) - 1);
        gate->invalidation_reason[sizeof(gate->invalidation_reason) - 1] = '\0';
    } else {
        strcpy(gate->invalidation_reason, "Unknown reason");
    }
    
    ALOGE("❌ AVSYNC INVALIDATED: %s", gate->invalidation_reason);
}

/// Проверить, открыт ли AVSYNC gate
///
/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH
/// Во время seek (seek_in_progress=true) gate всегда открыт для bypass AVSYNC
/// Это устраняет deadlock между seek / render / audio
bool avsync_gate_is_open(const AVSyncGate *gate) {
    if (!gate) {
        return false;
    }
    
    // 🔓 SEEK BYPASS: если seek в процессе, разрешаем render без ожидания master clock
    if (atomic_load(&gate->seek_in_progress)) {
        return true;
    }
    
    return atomic_load(&gate->master_valid);
}

/// Установить master clock type
void avsync_gate_set_master(AVSyncGate *gate, AvSyncMasterTypeGate master_type) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->master, master_type);
    
    const char *master_name = "NONE";
    switch (master_type) {
        case AVSYNC_MASTER_AUDIO_GATE:
            master_name = "AUDIO";
            break;
        case AVSYNC_MASTER_VIDEO_GATE:
            master_name = "VIDEO";
            break;
        case AVSYNC_MASTER_NONE_GATE:
            master_name = "NONE";
            break;
    }
    
    ALOGI("🎛 AVSYNC MASTER = %s", master_name);
}

/// Получить master clock type
AvSyncMasterTypeGate avsync_gate_get_master(const AVSyncGate *gate) {
    if (!gate) {
        return AVSYNC_MASTER_NONE_GATE;
    }
    
    return (AvSyncMasterTypeGate)atomic_load(&gate->master);
}

/// Установить master clock как валидный
void avsync_gate_set_valid(AVSyncGate *gate) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->master_valid, true);
    atomic_store(&gate->last_master_advance_us, get_time_us());
    
    AvSyncMasterTypeGate master = avsync_gate_get_master(gate);
    const char *master_name = (master == AVSYNC_MASTER_AUDIO_GATE) ? "AUDIO" : "VIDEO";
    ALOGI("✅ AVSYNC MASTER VALID = %s", master_name);
}

/// Обновить audio clock
void avsync_gate_update_audio_clock(AVSyncGate *gate, int64_t clock_us) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->audio_clock_us, clock_us);
    
    // Если audio master → обновляем last_master_advance_us
    if (avsync_gate_get_master(gate) == AVSYNC_MASTER_AUDIO_GATE && avsync_gate_is_open(gate)) {
        atomic_store(&gate->last_master_advance_us, get_time_us());
    }
}

/// Обновить video clock
void avsync_gate_update_video_clock(AVSyncGate *gate, int64_t clock_us) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->video_clock_us, clock_us);
    
    // Если video master → обновляем last_master_advance_us
    if (avsync_gate_get_master(gate) == AVSYNC_MASTER_VIDEO_GATE && avsync_gate_is_open(gate)) {
        atomic_store(&gate->last_master_advance_us, get_time_us());
    }
}

/// Получить audio clock
int64_t avsync_gate_get_audio_clock_us(const AVSyncGate *gate) {
    if (!gate) {
        return 0;
    }
    
    return atomic_load(&gate->audio_clock_us);
}

/// Получить video clock
int64_t avsync_gate_get_video_clock_us(const AVSyncGate *gate) {
    if (!gate) {
        return 0;
    }
    
    return atomic_load(&gate->video_clock_us);
}

/// Получить время последнего обновления master clock
int64_t avsync_gate_get_last_advance_us(const AVSyncGate *gate) {
    if (!gate) {
        return 0;
    }
    
    return atomic_load(&gate->last_master_advance_us);
}

/// Проверить clock stall (для watchdog)
bool avsync_gate_check_stall(const AVSyncGate *gate, int64_t stall_threshold_us) {
    if (!gate || !avsync_gate_is_open(gate)) {
        return false;
    }
    
    int64_t last_advance = avsync_gate_get_last_advance_us(gate);
    if (last_advance == 0) {
        return false;  // Ещё не было обновлений
    }
    
    int64_t now_us = get_time_us();
    int64_t elapsed_us = now_us - last_advance;
    
    if (elapsed_us > stall_threshold_us) {
        ALOGE("❌ AVSYNC CLOCK STALL: %lld ms ago", (long long)(elapsed_us / 1000));
        return true;
    }
    
    return false;
}

/// Установить seek_in_progress флаг
void avsync_gate_set_seek_in_progress(AVSyncGate *gate, bool in_progress) {
    if (!gate) {
        return;
    }
    
    atomic_store(&gate->seek_in_progress, in_progress);
    
    if (in_progress) {
        ALOGI("🔍 SEEK: AVSYNC disabled (seek_in_progress=true)");
    } else {
        ALOGI("🔓 SEEK DONE: AVSYNC restored");
    }
}

/// Проверить, идёт ли seek
bool avsync_gate_is_seek_in_progress(const AVSyncGate *gate) {
    if (!gate) {
        return false;
    }
    
    return atomic_load(&gate->seek_in_progress);
}

