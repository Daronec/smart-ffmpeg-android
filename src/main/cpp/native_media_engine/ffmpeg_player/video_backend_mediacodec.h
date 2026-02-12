/// Шаг 29: MediaCodec Video Decode (Surface / Hybrid Mode)
///
/// Использует MediaCodec для hardware video decode
/// FFmpeg остаётся для demux, audio, subtitles

#ifndef VIDEO_BACKEND_MEDIACODEC_H
#define VIDEO_BACKEND_MEDIACODEC_H

#include <jni.h>
#include "packet_queue.h"
#include "clock.h"
#include <stdbool.h>

// Forward declarations (полные определения в audio_renderer.h)
// AudioState определен как typedef struct { ... } AudioState; в audio_renderer.h
// Используем просто forward declaration без typedef
struct AudioState;

/// MediaCodec video backend
///
/// Декодирует видео через MediaCodec в Surface mode
/// Интегрируется с существующим frame_queue и audio clock
typedef struct VideoBackendMediaCodec {
    /// JavaVM для JNI
    JavaVM *jvm;
    
    /// MediaCodec объект (Java)
    jobject media_codec;
    
    /// Surface для рендеринга (из Flutter Texture)
    jobject surface;
    
    /// SurfaceTexture (из Flutter)
    jobject surface_texture;
    
    /// Texture ID для Flutter
    int64_t texture_id;
    
    /// Очередь пакетов (из demux thread)
    PacketQueue *packet_queue;
    
    /// Audio state (для A/V sync)
    struct AudioState *audio_state;
    
    /// Video clock (slave к audio)
    Clock video_clock;
    
    /// Ширина видео
    int width;
    
    /// Высота видео
    int height;
    
    /// Codec ID (AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, etc.)
    int codec_id;
    
    /// MIME type ("video/avc", "video/hevc", etc.)
    char mime[32];
    
    /// Флаг инициализации
    bool initialized;
    
    /// Флаг паузы
    bool paused;
    
    /// Флаг прерывания
    int abort;
    
    /// Thread для декодирования
    pthread_t decode_thread;
} VideoBackendMediaCodec;

/// Инициализировать MediaCodec backend
///
/// @param backend Backend для инициализации
/// @param jvm JavaVM для JNI
/// @param texture_id Texture ID из Flutter
/// @param codec_id FFmpeg codec ID
/// @param width Ширина видео
/// @param height Высота видео
/// @param packet_queue Очередь пакетов
/// @param audio_state Audio state для sync
/// @return 0 при успехе, <0 при ошибке
int video_backend_mediacodec_init(VideoBackendMediaCodec *backend,
                                   JavaVM *jvm,
                                   int64_t texture_id,
                                   int codec_id,
                                   int width,
                                   int height,
                                   PacketQueue *packet_queue,
                                   struct AudioState *audio_state);

/// Запустить декодирование
///
/// @param backend Backend
/// @return 0 при успехе, <0 при ошибке
int video_backend_mediacodec_start(VideoBackendMediaCodec *backend);

/// Приостановить декодирование
///
/// @param backend Backend
void video_backend_mediacodec_pause(VideoBackendMediaCodec *backend);

/// Возобновить декодирование
///
/// @param backend Backend
void video_backend_mediacodec_resume(VideoBackendMediaCodec *backend);

/// Выполнить seek
///
/// @param backend Backend
/// @param pts Позиция в секундах
/// @return 0 при успехе, <0 при ошибке
int video_backend_mediacodec_seek(VideoBackendMediaCodec *backend, double pts);

/// Остановить декодирование
///
/// @param backend Backend
void video_backend_mediacodec_stop(VideoBackendMediaCodec *backend);

/// Очистить буферы (flush)
///
/// @param backend Backend
void video_backend_mediacodec_flush(VideoBackendMediaCodec *backend);

/// Освободить ресурсы
///
/// @param backend Backend
void video_backend_mediacodec_release(VideoBackendMediaCodec *backend);

/// Проверить, инициализирован ли backend
///
/// @param backend Backend
/// @return true если инициализирован
bool video_backend_mediacodec_is_initialized(VideoBackendMediaCodec *backend);

/// Получить текущий video clock
///
/// @param backend Backend
/// @return Video clock в секундах
double video_backend_mediacodec_get_clock(VideoBackendMediaCodec *backend);

#endif // VIDEO_BACKEND_MEDIACODEC_H

