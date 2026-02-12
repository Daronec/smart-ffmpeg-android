#include "video_render_android.h"
#include "libavutil/pixdesc.h"  // для av_get_pix_fmt_name
#include <android/log.h>
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "VideoRender"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

bool video_render_init(VideoRenderAndroid *vr,
                       JavaVM *jvm,
                       ANativeWindow *window,
                       int src_w,
                       int src_h,
                       enum AVPixelFormat src_pix_fmt,
                       AVRational time_base) {
    memset(vr, 0, sizeof(*vr));
    
    vr->jvm = jvm;
    vr->window = window;
    vr->src_w = src_w;
    vr->src_h = src_h;
    vr->time_base = time_base;
    
    if (!window) {
        LOGE("ANativeWindow is null");
        return false;
    }
    
    // Настраиваем размеры окна
    vr->dst_w = src_w;
    vr->dst_h = src_h;
    
    // Настраиваем формат буферов (RGBA_8888)
    if (ANativeWindow_setBuffersGeometry(
            window,
            vr->dst_w,
            vr->dst_h,
            WINDOW_FORMAT_RGBA_8888) < 0) {
        LOGE("Failed to set buffers geometry");
        return false;
    }
    
    // 🎯 Шаг 25.5: Zero-copy - НЕ выделяем промежуточный rgba_buf
    // swscale будет писать напрямую в ANativeWindow buffer
    vr->rgba_buf = NULL; // Не используем промежуточный буфер
    vr->rgba_stride = 0;
    
    // Создаём SwsContext для конвертации YUV → RGBA (Шаг 25.5)
    vr->sws = sws_getContext(
        src_w,
        src_h,
        src_pix_fmt,        // исходный формат (YUV420P, etc.)
        vr->dst_w,
        vr->dst_h,
        AV_PIX_FMT_RGBA,    // целевой формат (RGBA для ANativeWindow)
        SWS_BILINEAR,       // алгоритм масштабирования
        NULL,
        NULL,
        NULL
    );
    
    if (!vr->sws) {
        LOGE("Failed to create SwsContext");
        return false;
    }
    
    vr->initialized = true;
    LOGI("VideoRender initialized (%dx%d, pix_fmt=%s -> RGBA)",
         src_w, src_h, av_get_pix_fmt_name(src_pix_fmt));
    
    return true;
}

void video_render_frame(VideoRenderAndroid *vr, AVFrame *frame) {
    if (!vr->initialized || !vr->window || !frame) {
        return;
    }
    
    // 🎯 Шаг 25.5: Software decoder zero-copy (прямой вывод в ANativeWindow)
    // Блокируем ANativeWindow для записи (один раз, не на каждый кадр)
    ANativeWindow_Buffer buffer;
    if (ANativeWindow_lock(vr->window, &buffer, NULL) < 0) {
        LOGE("Failed to lock ANativeWindow");
        return;
    }
    
    // Zero-copy: swscale напрямую в ANativeWindow buffer
    uint8_t *dst_data[1] = { (uint8_t *)buffer.bits };
    int dst_linesize[1] = { buffer.stride * 4 }; // RGBA = 4 bytes per pixel
    
    int ret = sws_scale(
        vr->sws,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        dst_data,
        dst_linesize
    );
    
    if (ret < 0) {
        LOGE("sws_scale failed: %d", ret);
        ANativeWindow_unlockAndPost(vr->window);
        return;
    }
    
    // Разблокируем и отправляем кадр (Шаг 25.6)
    // Flutter Texture автоматически обновится через SurfaceTexture.updateTexImage()
    ANativeWindow_unlockAndPost(vr->window);
}

void video_render_release(VideoRenderAndroid *vr) {
    if (!vr) {
        return;
    }
    
    if (vr->sws) {
        sws_freeContext(vr->sws);
        vr->sws = NULL;
    }
    
    // Шаг 25.5: rgba_buf больше не используется (zero-copy)
    // if (vr->rgba_buf) {
    //     free(vr->rgba_buf);
    //     vr->rgba_buf = NULL;
    // }
    
    if (vr->window) {
        ANativeWindow_release(vr->window);
        vr->window = NULL;
    }
    
    vr->initialized = false;
    LOGI("VideoRender released");
}

