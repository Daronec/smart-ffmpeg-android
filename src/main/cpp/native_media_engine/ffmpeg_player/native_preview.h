#ifndef NATIVE_PREVIEW_H
#define NATIVE_PREVIEW_H

#include <stdint.h>

/// 🔥 КРИТИЧЕСКИЙ FIX: PreviewContext - отдельный контекст для preview (не зависит от PlayerContext)
/// PreviewContext используется ТОЛЬКО для генерации превью кадров (CPU-only, без EGL/Surface/threads)
/// Создаётся → используется → уничтожается за один вызов getPreviewFrame()
typedef struct PreviewContext {
    void *fmt;  // AVFormatContext* (opaque pointer)
    void *dec;  // AVCodecContext* (opaque pointer)
    void *frame;  // AVFrame* (opaque pointer)
    void *rgb;  // AVFrame* (opaque pointer) для RGBA
    void *pkt;  // AVPacket* (opaque pointer)
    void *sws;  // SwsContext* (opaque pointer) для конвертации в RGBA
    int video_stream;  // Индекс видео потока
} PreviewContext;

/// 🔥 КРИТИЧЕСКИЙ FIX: Получить preview кадр (RGBA8888 bitmap)
/// 
/// Алгоритм:
/// 1. Открыть файл
/// 2. Найти видео поток
/// 3. Открыть декодер
/// 4. Seek BACKWARD к target_ms
/// 5. Декодировать кадры вперёд до первого >= target_ms
/// 6. Конвертировать в RGBA
/// 7. Вернуть bitmap
/// 
/// ❌ НЕ использует:
/// - PlayerContext
/// - EGL / Surface
/// - Render loop
/// - Threads
/// - AVSYNC-GATE
/// - FSM
/// 
/// @param path Путь к видео файлу
/// @param target_ms Целевая позиция в миллисекундах
/// @param out_w Ширина выходного bitmap
/// @param out_h Высота выходного bitmap
/// @param buffer Выходной буфер (RGBA8888, размер = out_w * out_h * 4)
/// @param buffer_size Размер буфера (должен быть >= out_w * out_h * 4)
/// @return 0 при успехе, < 0 при ошибке
int native_preview_get_frame(
    const char *path,
    int64_t target_ms,
    int out_w,
    int out_h,
    uint8_t *buffer,
    int buffer_size
);

#endif // NATIVE_PREVIEW_H

