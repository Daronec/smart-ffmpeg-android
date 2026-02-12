#include "clock.h"
#include <time.h>
#include <string.h>
#include <math.h>  // Для NAN

/// Получить текущее время (monotonic clock, секунды)
static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void clock_init(Clock *c) {
    memset(c, 0, sizeof(Clock));
    pthread_mutex_init(&c->mutex, NULL);
    // 🔴 ЭТАЛОН: video_clock инициализируется как NAN, не 0.0
    // Первый валидный PTS = старт video_clock
    c->pts = NAN;
    c->pts_drift = 0.0;
    c->last_updated = now_sec();
    c->active = false;
    c->paused = 0;
    c->speed = 1.0; // Шаг 39.2: Нормальная скорость по умолчанию
    c->latency = 0.0; // ШАГ 4: Latency по умолчанию 0
}

void clock_set(Clock *c, double pts) {
    if (!c) {
        return;
    }
    
    double now = now_sec();
    
    pthread_mutex_lock(&c->mutex);
    c->pts = pts;
    c->last_updated = now;
    c->pts_drift = c->pts - now; // Шаг 36.1
    c->active = true;
    pthread_mutex_unlock(&c->mutex);
}

double clock_get(Clock *c) {
    if (!c) {
        return 0.0;
    }
    
    pthread_mutex_lock(&c->mutex);
    
    if (!c->active) {
        pthread_mutex_unlock(&c->mutex);
        return 0.0;
    }
    
    double pts = c->pts;
    double delta = now_sec() - c->last_updated;
    
    pthread_mutex_unlock(&c->mutex);
    
    return pts + delta;
}

void clock_reset(Clock *c, double pts) {
    if (!c) {
        return;
    }
    
    double now = now_sec();
    
    pthread_mutex_lock(&c->mutex);
    c->pts = pts;
    c->pts_drift = pts - now; // Шаг 36.10, 39.6: Вычисляем drift
    c->last_updated = now;
    c->active = (pts > 0.0);
    c->paused = 0;
    // speed не сбрасываем при reset (Шаг 39.6)
    pthread_mutex_unlock(&c->mutex);
}

bool clock_is_active(Clock *c) {
    if (!c) {
        return false;
    }
    
    pthread_mutex_lock(&c->mutex);
    bool active = c->active;
    pthread_mutex_unlock(&c->mutex);
    
    return active;
}

void clock_pause(Clock *c, int pause) {
    if (!c) {
        return;
    }
    
    pthread_mutex_lock(&c->mutex);
    
    // Шаг 36.9: Pause / Resume
    if (pause && !c->paused) {
        // При паузе: сохраняем текущий PTS и обновляем last_updated
        // Это гарантирует, что при resume clock продолжит правильно
        double now = now_sec();
        double delta = (now - c->last_updated) * c->speed;
        c->pts = c->pts + delta;  // Обновляем pts до текущего момента
        c->last_updated = now;     // Обновляем last_updated для корректного resume
    }
    
    c->paused = pause;
    
    pthread_mutex_unlock(&c->mutex);
}

double clock_get_time(Clock *c) {
    if (!c) {
        return 0.0;
    }
    
    double now = now_sec();
    
    pthread_mutex_lock(&c->mutex);
    
    if (!c->active) {
        pthread_mutex_unlock(&c->mutex);
        return 0.0;
    }
    
    // Шаг 36.3, 39.2: Получение master time с учётом паузы и speed
    double pts;
    if (c->paused) {
        pts = c->pts;  // При паузе возвращаем сохранённый PTS
    } else {
        // При воспроизведении: pts = pts_drift + now + (now - last_updated) * (speed - 1.0)
        // Это означает: время идёт со скоростью speed
        double delta = (now - c->last_updated) * c->speed;
        pts = c->pts + delta;
    }
    
    // ШАГ 4: Учитываем latency (если установлен)
    // latency вычитается из pts для получения реального времени воспроизведения
    if (c->latency > 0.0) {
        pts -= c->latency;
    }
    
    pthread_mutex_unlock(&c->mutex);
    
    return pts;
}

void clock_set_speed(Clock *c, double speed) {
    if (!c) {
        return;
    }
    
    // Шаг 39.2: Ограничиваем скорость (0.5x .. 3.0x)
    if (speed < 0.5) {
        speed = 0.5;
    } else if (speed > 3.0) {
        speed = 3.0;
    }
    
    pthread_mutex_lock(&c->mutex);
    
    // Шаг 39.6: При изменении speed обновляем pts и last_updated
    if (c->active && !c->paused) {
        double now = now_sec();
        // Пересчитываем текущий pts с учётом старой скорости
        double delta_old = (now - c->last_updated) * c->speed;
        c->pts = c->pts + delta_old; // Обновляем pts до текущего момента
        c->last_updated = now; // Сбрасываем last_updated для новой скорости
    }
    
    c->speed = speed;
    
    pthread_mutex_unlock(&c->mutex);
}

double clock_get_speed(Clock *c) {
    if (!c) {
        return 1.0;
    }
    
    pthread_mutex_lock(&c->mutex);
    double speed = c->speed;
    pthread_mutex_unlock(&c->mutex);
    
    return speed;
}

void clock_set_latency(Clock *c, double latency) {
    if (!c) {
        return;
    }
    
    pthread_mutex_lock(&c->mutex);
    c->latency = latency;
    pthread_mutex_unlock(&c->mutex);
}

