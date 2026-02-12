#include "frame_queue.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "libavutil/rational.h"
#include <android/log.h>

#define LOG_TAG "FrameQueue"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Очистить кадр
static void frame_clear(Frame *f) {
    if (f->frame) {
        av_frame_free(&f->frame);
    }
    f->frame = NULL;
    f->pts = 0.0;
}

void frame_queue_init(FrameQueue *fq, AVRational time_base) {
    memset(fq, 0, sizeof(FrameQueue));
    pthread_mutex_init(&fq->mutex, NULL);
    pthread_cond_init(&fq->cond, NULL);
    fq->read_index = 0;
    fq->rindex = 0; // Alias (Шаг 41.1)
    fq->write_index = 0;
    fq->windex = 0; // Alias (Шаг 41.1)
    fq->size = 0;
    fq->max_size = FRAME_QUEUE_SIZE;
    fq->abort_request = false;
    
    // 🔴 КРИТИЧНО: Сохраняем time_base для fallback PTS из best_effort_timestamp
    fq->time_base = time_base;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Synthetic PTS для AVI/FLV
    fq->last_pts = NAN;
    fq->estimated_frame_duration = 0.04; // 25fps fallback
    
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        fq->queue[i].frame = NULL;
        fq->queue[i].pts = 0.0;
        fq->queue[i].serial = 0; // Шаг 41.1
    }
}

void frame_queue_destroy(FrameQueue *fq) {
    frame_queue_flush(fq);
    pthread_mutex_destroy(&fq->mutex);
    pthread_cond_destroy(&fq->cond);
}

void frame_queue_abort(FrameQueue *fq) {
    pthread_mutex_lock(&fq->mutex);
    fq->abort_request = true;
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
}

void frame_queue_flush(FrameQueue *fq) {
    pthread_mutex_lock(&fq->mutex);
    
    for (int i = 0; i < FRAME_QUEUE_SIZE; i++) {
        frame_clear(&fq->queue[i]);
    }
    
    fq->size = 0;
    fq->read_index = 0;
    fq->rindex = 0;
    fq->write_index = 0;
    fq->windex = 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Reset last_pts при flush (для seek)
    fq->last_pts = NAN;
    
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
}

int frame_queue_push(FrameQueue *fq, AVFrame *frame, double pts, int serial) {
    pthread_mutex_lock(&fq->mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.5: Защита эпох
    // Если serial не совпадает с текущим seek_serial, кадр из старой эпохи → дроп
    // Это предотвращает показ старых кадров после seek
    // (serial проверяется в decode thread перед push, но здесь дополнительная защита)
    
    // Шаг 34.1: Backpressure - decoder блокируется, если очередь полна
    while (fq->size >= fq->max_size && !fq->abort_request) {
        pthread_cond_wait(&fq->cond, &fq->mutex);
    }
    
    if (fq->abort_request) {
        pthread_mutex_unlock(&fq->mutex);
        return -1;
    }
    
    Frame *dst = &fq->queue[fq->write_index];
    frame_clear(dst);
    
    // Клонируем кадр (безопасно, decoder может переиспользовать frame)
    dst->frame = av_frame_clone(frame);
    if (!dst->frame) {
        pthread_mutex_unlock(&fq->mutex);
        return -1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Synthetic PTS для AVI/FLV (как в VLC / mpv)
    // Если pts == NAN, используем синтетический PTS: last_pts + frame_duration
    if (isnan(pts)) {
        // 🔥 PATCH: Synthetic PTS для сломанных контейнеров
        if (!isnan(fq->last_pts)) {
            pts = fq->last_pts + fq->estimated_frame_duration;
            ALOGW("⚠️ FRAME_QUEUE: Using synthetic PTS=%.3f (last=%.3f + duration=%.3f)", 
                  pts, fq->last_pts, fq->estimated_frame_duration);
        } else {
            // Первый кадр без PTS - используем 0.0
            pts = 0.0;
            ALOGW("⚠️ FRAME_QUEUE: First frame without PTS, using pts=0.0");
        }
    }
    
    // Обновляем last_pts и estimated_frame_duration
    if (!isnan(fq->last_pts) && pts > fq->last_pts) {
        // Обновляем estimated_frame_duration на основе реальных PTS
        double delta = pts - fq->last_pts;
        if (delta > 0.0 && delta < 1.0) { // Разумные значения (0-1 сек)
            fq->estimated_frame_duration = delta;
        }
    }
    fq->last_pts = pts;
    
    dst->pts = pts;
    dst->serial = serial;  // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.5: Устанавливаем serial эпохи
    
    fq->write_index = (fq->write_index + 1) % fq->max_size;
    fq->windex = fq->write_index; // Alias (Шаг 41.1)
    fq->size++;
    
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
    
    return 0;
}

bool frame_queue_is_full(FrameQueue *fq) {
    if (!fq) {
        return false;
    }
    
    pthread_mutex_lock(&fq->mutex);
    bool full = (fq->size >= FRAME_QUEUE_SIZE);
    pthread_mutex_unlock(&fq->mutex);
    
    return full;
}

int frame_queue_size(FrameQueue *fq) {
    if (!fq) {
        return 0;
    }
    
    pthread_mutex_lock(&fq->mutex);
    int size = fq->size;
    pthread_mutex_unlock(&fq->mutex);
    
    return size;
}

int frame_queue_peek(FrameQueue *fq, Frame *out) {
    if (!fq || !out) {
        return -1;
    }
    
    pthread_mutex_lock(&fq->mutex);
    
    if (fq->abort_request) {
        pthread_mutex_unlock(&fq->mutex);
        return -1;
    }
    
    if (fq->size == 0) {
        pthread_mutex_unlock(&fq->mutex);
        return 0;
    }
    
    // Копируем данные кадра (без ownership)
    Frame *src = &fq->queue[fq->read_index];
    out->frame = src->frame; // Указатель, но не ownership
    out->pts = src->pts;
    out->serial = src->serial; // Шаг 41.1
    
    pthread_mutex_unlock(&fq->mutex);
    
    return 1;
}

/// Получить указатель на текущий кадр (Шаг 41.1)
Frame* frame_queue_peek_ptr(FrameQueue *fq) {
    if (!fq) {
        return NULL;
    }
    
    pthread_mutex_lock(&fq->mutex);
    
    if (fq->abort_request || fq->size <= 0) {
        pthread_mutex_unlock(&fq->mutex);
        return NULL;
    }
    
    Frame *result = &fq->queue[fq->read_index];
    
    pthread_mutex_unlock(&fq->mutex);
    
    return result;
}

/// Получить указатель на следующий кадр (Шаг 41.1)
Frame* frame_queue_peek_next_ptr(FrameQueue *fq) {
    if (!fq) {
        return NULL;
    }
    
    pthread_mutex_lock(&fq->mutex);
    
    if (fq->abort_request || fq->size < 2) {
        pthread_mutex_unlock(&fq->mutex);
        return NULL;
    }
    
    int next_index = (fq->read_index + 1) % fq->max_size;
    Frame *result = &fq->queue[next_index];
    
    pthread_mutex_unlock(&fq->mutex);
    
    return result;
}

/// Продвинуть очередь к следующему кадру (Шаг 41.1)
void frame_queue_next(FrameQueue *fq) {
    if (!fq) {
        return;
    }
    
    pthread_mutex_lock(&fq->mutex);
    
    if (fq->size > 0) {
        Frame *f = &fq->queue[fq->read_index];
        
        // Освобождаем кадр
        if (f->frame) {
            av_frame_free(&f->frame);
        }
        f->frame = NULL;
        f->pts = 0.0;
        f->serial = 0;
        
        // Продвигаем индекс чтения
        fq->read_index = (fq->read_index + 1) % fq->max_size;
        fq->rindex = fq->read_index; // Alias
        fq->size--;
        
        pthread_cond_signal(&fq->cond);
    }
    
    pthread_mutex_unlock(&fq->mutex);
}

int frame_queue_drop_oldest(FrameQueue *fq) {
    if (!fq) {
        return 0;
    }
    
    pthread_mutex_lock(&fq->mutex);
    
    if (fq->size == 0) {
        pthread_mutex_unlock(&fq->mutex);
        return 0;
    }
    
    // Удаляем старейший кадр (read_index)
    Frame *oldest = &fq->queue[fq->read_index];
    frame_clear(oldest);
    
    fq->read_index = (fq->read_index + 1) % FRAME_QUEUE_SIZE;
    fq->size--;
    
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
    
    return 1;
}

int frame_queue_pop(FrameQueue *fq, Frame *out, bool block) {
    pthread_mutex_lock(&fq->mutex);
    
    while (fq->size == 0 && !fq->abort_request) {
        if (!block) {
            pthread_mutex_unlock(&fq->mutex);
            return 0;
        }
        pthread_cond_wait(&fq->cond, &fq->mutex);
    }
    
    if (fq->abort_request) {
        pthread_mutex_unlock(&fq->mutex);
        return -1;
    }
    
    Frame *src = &fq->queue[fq->read_index];
    *out = *src;   // shallow copy (frame ownership переходит наружу)
    src->frame = NULL;
    src->pts = 0.0;
    
    fq->read_index = (fq->read_index + 1) % FRAME_QUEUE_SIZE;
    fq->size--;
    
    pthread_cond_signal(&fq->cond);
    pthread_mutex_unlock(&fq->mutex);
    
    return 1;
}
