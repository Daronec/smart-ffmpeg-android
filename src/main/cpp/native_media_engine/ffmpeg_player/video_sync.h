#pragma once

#include "clock.h"
#include <stdbool.h>

/// Пороги для A/V синхронизации (Шаг 36.6)
#define AV_SYNC_THRESHOLD_MIN 0.04   // 40 ms - минимальный порог
#define AV_SYNC_THRESHOLD_MAX 0.1    // 100 ms - максимальный порог
#define AV_SYNC_DROP_THRESHOLD 0.3   // 300 ms - порог для drop frame

/// Результат синхронизации
typedef enum {
    VIDEO_SYNC_RENDER,  // Показать кадр
    VIDEO_SYNC_DROP,    // Отбросить кадр
    VIDEO_SYNC_SLEEP    // Подождать перед показом
} VideoSyncResult;

/// Синхронизировать видео к аудио (master clock)
///
/// @param video_pts PTS видео кадра (в секундах)
/// @param audio_clock Audio clock (master clock)
/// @return Результат синхронизации
VideoSyncResult video_sync_and_wait(double video_pts, double audio_clock);

/// Вычислить delay для синхронизации
///
/// @param video_pts PTS видео кадра
/// @param audio_clock Audio clock
/// @return Delay в секундах (положительный = sleep, отрицательный = drop)
double video_sync_compute_delay(double video_pts, double audio_clock);

