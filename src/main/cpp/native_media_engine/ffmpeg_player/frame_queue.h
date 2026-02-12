#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <math.h>
#include "libavutil/frame.h"
#include "libavutil/rational.h"

/// Размер ring buffer для кадров (Шаг 41.1 - увеличен для interpolation)
#define FRAME_QUEUE_SIZE 16

/// Кадр в очереди (Шаг 41.1)
///
/// Содержит декодированный AVFrame и метаданные
typedef struct Frame {
    /// Декодированный кадр (ownership переходит при pop/next)
    AVFrame *frame;
    
    /// PTS кадра (в секундах)
    double pts;
    
    /// Serial number (для отслеживания seek, Шаг 41.1)
    int serial;
} Frame;

/// Очередь кадров (thread-safe, ring buffer, Шаг 41.1)
///
/// Используется для передачи decoded frames между потоками:
/// - decode threads → render threads
/// - ring buffer (не malloc/free каждый кадр)
/// - thread-safe через mutex + cond
/// - bounded (FRAME_QUEUE_SIZE)
/// - abort-safe
/// - Поддержка interpolation (peek/peek_next/next)
typedef struct FrameQueue {
    /// Ring buffer кадров
    Frame queue[FRAME_QUEUE_SIZE];
    
    /// Индекс чтения (rindex для совместимости с ffplay, Шаг 41.1)
    int read_index;
    int rindex; // Alias для совместимости
    
    /// Индекс записи (windex для совместимости с ffplay, Шаг 41.1)
    int write_index;
    int windex; // Alias для совместимости
    
    /// Текущий размер очереди
    int size;
    
    /// Максимальный размер очереди
    int max_size;
    
    /// Флаг прерывания
    bool abort_request;
    
    /// Mutex для синхронизации
    pthread_mutex_t mutex;
    
    /// Condition variable для ожидания
    pthread_cond_t cond;
    
    /// 🔴 КРИТИЧНО: time_base для конвертации PTS из best_effort_timestamp
    /// Используется как fallback, если переданный pts невалиден
    AVRational time_base;
    
    /// 🔥 КРИТИЧЕСКИЙ FIX: Synthetic PTS для AVI/FLV
    /// Последний PTS кадра в очереди (для синтетического PTS при isnan(pts))
    double last_pts;
    
    /// 🔥 КРИТИЧЕСКИЙ FIX: Estimated frame duration для synthetic PTS
    /// Используется для синтетического PTS: pts = last_pts + frame_duration
    double estimated_frame_duration;
} FrameQueue;

/// Инициализировать очередь кадров
///
/// @param fq Очередь для инициализации
/// @param time_base time_base для конвертации PTS (может быть {0,0} если неизвестен)
void frame_queue_init(FrameQueue *fq, AVRational time_base);

/// Освободить ресурсы очереди
///
/// @param fq Очередь
void frame_queue_destroy(FrameQueue *fq);

/// Прервать очередь (для stop/destroy)
///
/// @param fq Очередь
void frame_queue_abort(FrameQueue *fq);

/// Очистить очередь (для seek/stop)
///
/// @param fq Очередь
void frame_queue_flush(FrameQueue *fq);

/// Добавить кадр в очередь
///
/// @param fq Очередь
/// @param frame Кадр для добавления (будет клонирован через av_frame_clone)
/// @param pts PTS кадра в секундах
/// @param serial Serial number для фильтрации старых эпох seek (ШАГ 10.5)
/// @return 0 при успехе, <0 при ошибке
int frame_queue_push(FrameQueue *fq, AVFrame *frame, double pts, int serial);

/// Извлечь кадр из очереди (блокирующий или неблокирующий)
///
/// @param fq Очередь
/// @param out Буфер для кадра (ownership переходит вызывающему)
/// @param block true = блокирующий (ждёт кадр), false = неблокирующий
/// @return 1 при успехе, 0 если очередь пуста (block=false), <0 при abort
int frame_queue_pop(FrameQueue *fq, Frame *out, bool block);

/// Проверить, полна ли очередь (Шаг 34.1 - Backpressure)
///
/// @param fq Очередь
/// @return true если очередь полна
bool frame_queue_is_full(FrameQueue *fq);

/// Получить размер очереди
///
/// @param fq Очередь
/// @return Текущий размер очереди
int frame_queue_size(FrameQueue *fq);

/// Просмотреть следующий кадр без извлечения (Шаг 33.4 - Frame pacing)
///
/// @param fq Очередь
/// @param out Буфер для кадра (только чтение, ownership НЕ переходит)
/// @return 1 при успехе, 0 если очередь пуста, <0 при abort
int frame_queue_peek(FrameQueue *fq, Frame *out);

/// Получить указатель на текущий кадр (Шаг 41.1 - для interpolation)
///
/// Возвращает указатель на Frame в очереди (без ownership)
/// Кадр остаётся в очереди до вызова frame_queue_next()
///
/// @param fq Очередь
/// @return Указатель на текущий кадр, или NULL если очередь пуста
Frame* frame_queue_peek_ptr(FrameQueue *fq);

/// Получить указатель на следующий кадр (Шаг 41.1 - для interpolation)
///
/// Возвращает указатель на следующий Frame в очереди (без ownership)
/// Используется для interpolation между текущим и следующим кадром
///
/// @param fq Очередь
/// @return Указатель на следующий кадр, или NULL если нет следующего
Frame* frame_queue_peek_next_ptr(FrameQueue *fq);

/// Продвинуть очередь к следующему кадру (Шаг 41.1 - для interpolation)
///
/// Освобождает текущий кадр и переходит к следующему
/// Вызывается после рендеринга кадра
///
/// @param fq Очередь
void frame_queue_next(FrameQueue *fq);

/// Удалить старейший кадр (Шаг 34.2 - Drop policy для video)
///
/// @param fq Очередь
/// @return 1 если кадр удалён, 0 если очередь пуста
int frame_queue_drop_oldest(FrameQueue *fq);
