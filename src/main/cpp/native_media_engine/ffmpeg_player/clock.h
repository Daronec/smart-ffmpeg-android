#pragma once

#include <pthread.h>
#include <stdbool.h>

/// Clock для синхронизации аудио и видео (Шаг 36.1, 39.2)
///
/// Используется для master clock (audio) и slave clock (video).
/// Thread-safe через mutex.
/// Поддерживает playback speed (Шаг 39.2)
typedef struct Clock {
    /// Mutex для синхронизации
    pthread_mutex_t mutex;
    
    /// Текущий PTS (в секундах)
    double pts;
    
    /// PTS drift (pts - system_time, Шаг 36.1)
    double pts_drift;
    
    /// Время последнего обновления (в секундах, monotonic clock)
    double last_updated;
    
    /// Флаг, что clock активен
    bool active;
    
    /// Флаг паузы (Шаг 36.9)
    int paused;
    
    /// Скорость воспроизведения (Шаг 39.2)
    /// 1.0 = нормальная скорость, 2.0 = 2x, 0.5 = 0.5x
    double speed;
    
    /// Audio latency в секундах (ШАГ 4)
    /// Учитывается при получении clock_get_time для точного audio clock
    double latency;
} Clock;

/// Инициализировать clock
///
/// @param c Clock для инициализации
void clock_init(Clock *c);

/// Установить PTS
///
/// @param c Clock
/// @param pts PTS в секундах
void clock_set(Clock *c, double pts);

/// Получить текущий clock (с учётом времени с последнего обновления)
///
/// @param c Clock
/// @return Текущий clock в секундах
double clock_get(Clock *c);

/// Сбросить clock
///
/// @param c Clock
/// @param pts Новый PTS (обычно 0 или seek_pos)
void clock_reset(Clock *c, double pts);

/// Проверить, активен ли clock
///
/// @param c Clock
/// @return true если активен
bool clock_is_active(Clock *c);

/// Установить/снять паузу (Шаг 36.9)
///
/// @param c Clock
/// @param pause 1 = пауза, 0 = воспроизведение
void clock_pause(Clock *c, int pause);

/// Получить текущее время с учётом паузы (Шаг 36.3)
///
/// @param c Clock
/// @return Текущее время в секундах
double clock_get_time(Clock *c);

/// Установить скорость воспроизведения (Шаг 39.2)
///
/// @param c Clock
/// @param speed Скорость (1.0 = нормальная, 2.0 = 2x, 0.5 = 0.5x)
void clock_set_speed(Clock *c, double speed);

/// Получить скорость воспроизведения
///
/// @param c Clock
/// @return Текущая скорость
double clock_get_speed(Clock *c);

/// Установить audio latency (ШАГ 4)
///
/// @param c Clock
/// @param latency Latency в секундах
void clock_set_latency(Clock *c, double latency);

