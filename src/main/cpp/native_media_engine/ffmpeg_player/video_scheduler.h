#pragma once

#include <stdbool.h>
#include "video_renderer.h"
#include "audio_renderer.h"
#include "frame_queue.h"
#include "video_render_gl.h"

/// VSync-driven video scheduler (ШАГ 1)
///
/// Управляет выводом кадров строго по времени экрана через AChoreographer.
/// Обеспечивает:
/// - VSync-aligned frame scheduling
/// - Frame interpolation (ШАГ 2)
/// - Audio clock как master clock (ШАГ 3)
/// - Точный audio clock с учётом latency (ШАГ 4)

/// Состояние VSync scheduler
typedef struct VideoScheduler {
    /// Video state (для доступа к frame_queue)
    VideoState *video_state;
    
    /// Audio state (для master clock)
    AudioState *audio_state;
    
    /// Video renderer (OpenGL)
    VideoRenderGL *video_render;
    
    /// AChoreographer instance (для VSync)
    void *choreographer; // AChoreographer*
    
    /// Флаг активности
    bool active;
    
    /// Флаг прерывания
    bool abort;
    
    /// Thread для VSync callback
    pthread_t vsync_thread;
    
    /// Mutex для синхронизации
    pthread_mutex_t mutex;
} VideoScheduler;

/// Инициализировать VSync scheduler (ШАГ 1)
///
/// @param scheduler Scheduler для инициализации
/// @param video_state Video state
/// @param audio_state Audio state (для master clock)
/// @param video_render Video renderer
/// @return 0 при успехе, <0 при ошибке
int video_scheduler_init(VideoScheduler *scheduler,
                         VideoState *video_state,
                         AudioState *audio_state,
                         VideoRenderGL *video_render);

/// Запустить VSync-driven рендеринг (ШАГ 1)
///
/// @param scheduler Scheduler
/// @return 0 при успехе, <0 при ошибке
int video_scheduler_start(VideoScheduler *scheduler);

/// Остановить VSync-driven рендеринг
///
/// @param scheduler Scheduler
void video_scheduler_stop(VideoScheduler *scheduler);

/// Освободить ресурсы scheduler
///
/// @param scheduler Scheduler
void video_scheduler_release(VideoScheduler *scheduler);

/// Получить master clock (ШАГ 3, 4)
///
/// @param scheduler Scheduler
/// @return Текущий master clock в секундах
double video_scheduler_get_master_clock(VideoScheduler *scheduler);

