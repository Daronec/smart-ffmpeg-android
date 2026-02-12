#pragma once

#include <jni.h>
#include <android/native_window.h>
#include "libavutil/frame.h"
#include "libswscale/swscale.h"
#include <stdbool.h>

/// Нативный видеорендер для Android (ANativeWindow / Flutter Texture)
///
/// Прямой вывод кадров FFmpeg в ANativeWindow без MediaCodec и ffmpeg-kit.
/// Используется в video_render_thread для рендеринга декодированных кадров.
typedef struct VideoRenderAndroid {
    /// JavaVM для JNI
    JavaVM *jvm;
    
    /// ANativeWindow (из Surface/SurfaceTexture)
    ANativeWindow *window;
    
    /// SwsContext для конвертации YUV → RGBA
    struct SwsContext *sws;
    
    /// Исходные размеры (из видео)
    int src_w;
    int src_h;
    
    /// Целевые размеры (для рендеринга)
    int dst_w;
    int dst_h;
    
    /// Буфер для RGBA данных
    uint8_t *rgba_buf;
    
    /// Stride для RGBA буфера
    int rgba_stride;
    
    /// Флаг инициализации
    bool initialized;
    
    /// Time base для расчёта PTS
    AVRational time_base;
} VideoRenderAndroid;

/// Инициализировать видеорендер
///
/// @param vr Видеорендер
/// @param jvm JavaVM для JNI
/// @param window ANativeWindow (из Surface/SurfaceTexture)
/// @param src_w Ширина исходного видео
/// @param src_h Высота исходного видео
/// @param src_pix_fmt Исходный формат пикселей
/// @param time_base Time base для расчёта PTS
/// @return true при успехе, false при ошибке
bool video_render_init(VideoRenderAndroid *vr,
                       JavaVM *jvm,
                       ANativeWindow *window,
                       int src_w,
                       int src_h,
                       enum AVPixelFormat src_pix_fmt,
                       AVRational time_base);

/// Рендерить кадр
///
/// @param vr Видеорендер
/// @param frame Кадр для рендеринга
void video_render_frame(VideoRenderAndroid *vr, AVFrame *frame);

/// Освободить ресурсы видеорендера
///
/// @param vr Видеорендер
void video_render_release(VideoRenderAndroid *vr);

