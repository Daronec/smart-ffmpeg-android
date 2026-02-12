/// Шаг 24: MediaCodec get_format callback и обработка MediaCodec frames
/// Шаг 25: Zero-copy pipeline для MediaCodec

#include "video_renderer.h"
#include "hw_accel.h"
#include <android/log.h>
#include "libavcodec/avcodec.h"
#include "libavcodec/mediacodec.h"  // для AVMediaCodecBuffer
#include "libavutil/hwcontext_mediacodec.h"
#include "libavutil/pixdesc.h"  // для av_get_pix_fmt_name

#define LOG_TAG "VideoMediaCodec"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Get format callback для MediaCodec (Шаг 24.4)
///
/// Вызывается FFmpeg при инициализации декодера для выбора pixel format
enum AVPixelFormat mediacodec_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    
    ALOGD("mediacodec_get_format called");
    
    // Ищем AV_PIX_FMT_MEDIACODEC в списке поддерживаемых форматов
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_MEDIACODEC) {
            ALOGI("MediaCodec format selected");
            return AV_PIX_FMT_MEDIACODEC;
        }
    }
    
    // Если MediaCodec не поддерживается - возвращаем первый формат (fallback)
    ALOGD("MediaCodec format not available, using fallback: %s", av_get_pix_fmt_name(pix_fmts[0]));
    return pix_fmts[0];
}

/// Обработать MediaCodec frame (Шаг 24.5 + 25.4)
///
/// Для MediaCodec используем zero-copy: release buffer → Surface
/// НЕ делаем memcpy, НЕ используем swscale
int video_handle_mediacodec_frame(struct VideoState *vs, AVFrame *frame) {
    if (!vs || !frame) {
        return -1;
    }
    
    // Проверяем, что это MediaCodec frame
    if (frame->format != AV_PIX_FMT_MEDIACODEC) {
        return 0; // Не MediaCodec - обрабатываем как обычно
    }
    
    // Шаг 24.5 + 25.4: Zero-copy для MediaCodec
    // AVMediaCodecBuffer находится в frame->data[3]
    AVMediaCodecBuffer *buffer = (AVMediaCodecBuffer *)frame->data[3];
    
    if (!buffer) {
        ALOGE("MediaCodec buffer is NULL");
        return -1;
    }
    
    // Release buffer с флагом render=1 (Шаг 25.4)
    // Это отправляет кадр напрямую в Surface (zero-copy)
    int ret = av_mediacodec_release_buffer(buffer, 1); // 1 = render to Surface
    
    if (ret < 0) {
        ALOGE("Failed to release MediaCodec buffer: %d", ret);
        return ret;
    }
    
    ALOGD("MediaCodec frame released to Surface (zero-copy)");
    return 0;
}

