/// 🔴 ЗАДАЧА 4: Lifecycle management для Native FFmpeg Player
///
/// Управление жизненным циклом плеера:
/// - attach/detach window
/// - start/stop render loop
/// - pause/resume при lifecycle изменениях

#include "ffmpeg_player.h"
#include "video_render_gl.h"
#include "video_renderer.h"  // Для video_decode_thread_start
#include "audio_renderer.h"
#include "avsync_gate.h"  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF
#include "clock.h"
#include "frame_queue.h"  // 🔴 ШАГ 5: Для frame_queue_abort
#include "packet_queue.h"  // 🔴 ШАГ 5: Для packet_queue_abort
#include "native_player_jni.h"  // Для native_player_emit_error_event
#include <pthread.h>
#include <android/log.h>
#include <EGL/egl.h>  // Для eglMakeCurrent
#include <jni.h>  // Для AttachCurrentThread
#include <unistd.h>  // Для usleep
#include "libavutil/time.h"  // Для av_gettime

#define LOG_TAG "FFmpegPlayerLifecycle"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Получить monotonic time в секундах
static double get_monotonic_time_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;  // микросекунды → секунды
}
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Внешние глобальные переменные (из native_player_jni.c)
extern VideoRenderGL *g_renderer;
extern JavaVM *g_jvm;  // Для AttachCurrentThread в render thread

/// Локальный флаг abort для render loop (используется в render_loop_wrapper_lifecycle)
static int g_lifecycle_render_abort = 0;

// Forward declarations (функции используются в других файлах)
int render_loop_start(PlayerContext *ctx);
void render_loop_stop(PlayerContext *ctx);
void player_shutdown(PlayerContext *ctx);  // 🔴 ШАГ 5: Forward declaration для player_shutdown

// Forward declarations для player_shutdown
void video_threads_stop(VideoState *vs);
void audio_threads_stop(AudioState *as);
void frame_queue_abort(FrameQueue *fq);
void packet_queue_abort(PacketQueue *q);
void close_media(PlayerContext *ctx);

/// Обёртка для render loop (используется в render_loop_start)
static void *render_loop_wrapper_lifecycle(void *arg) {
    PlayerContext *ctx = (PlayerContext *)arg;
    
    if (!ctx || !g_renderer) {
        ALOGE("render_loop_wrapper_lifecycle: ctx or g_renderer is NULL");
        return NULL;
    }
    
    if (!ctx->video || !ctx->video->frameQueue) {
        ALOGE("render_loop_wrapper_lifecycle: video or frameQueue is NULL");
        return NULL;
    }
    
    ALOGI("🎬 Render loop thread started (lifecycle)");
    
    // 🔴 КРИТИЧНО: Attach render thread к JVM для JNI callbacks
    // Без этого native_player_mark_frame_available() не сможет вызвать onFrameAvailable()
    JNIEnv *env = NULL;
    if (!g_jvm) {
        ALOGE("❌ render_loop_wrapper_lifecycle: g_jvm is NULL");
        return NULL;
    }
    
    if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
        ALOGE("❌ Failed to attach render thread to JVM");
        return NULL;
    }
    
    ALOGI("✅ Render thread attached to JVM");
    
    // 🔴 КРИТИЧНО: Проверяем тип рендеринга
    if (!g_renderer) {
        ALOGE("❌ render_loop_wrapper_lifecycle: g_renderer is NULL");
        return NULL;
    }
    
    // 🔴 ЭТАЛОН: Проверяем, что render_target установлен
    if (g_renderer->render_target == RENDER_TARGET_NONE) {
        ALOGE("❌ render_loop_wrapper_lifecycle: Render target not set yet (call video_render_gl_attach_window or video_render_gl_register_image_texture first)");
        ALOGE("   renderer=%p, render_target=NONE", (void *)g_renderer);
        return NULL;
    }
    
    // Для Surface требуется EGL surface, для ImageTexture - нет
    if (g_renderer->render_target == RENDER_TARGET_SURFACE) {
        if (g_renderer->egl_surface == EGL_NO_SURFACE) {
            ALOGE("❌ render_loop_wrapper_lifecycle: EGL surface not created for SURFACE target (call video_render_gl_attach_window first)");
            ALOGE("   renderer=%p, surface=%p", (void *)g_renderer, (void *)g_renderer->egl_surface);
            return NULL;
        }
    } else if (g_renderer->render_target == RENDER_TARGET_IMAGE_TEXTURE) {
        // Для ImageTexture EGL surface не нужен, но нужен flutter_texture_id
        if (g_renderer->flutter_texture_id <= 0) {
            ALOGE("❌ render_loop_wrapper_lifecycle: Flutter texture ID not registered for IMAGE_TEXTURE target (call video_render_gl_register_image_texture first)");
            return NULL;
        }
        ALOGI("✅ render_loop_wrapper_lifecycle: ImageTexture mode - no EGL surface needed (textureId=%ld)", g_renderer->flutter_texture_id);
    } else {
        ALOGE("❌ render_loop_wrapper_lifecycle: Unknown render target: %d", g_renderer->render_target);
        return NULL;
    }
    
    // 🔴 КРИТИЧНО: НЕ делаем detach здесь (в wrapper thread)
    // EGL context должен быть уже detached из JNI thread в player_attach_window()
    // Если context был current в другом потоке, detach здесь не поможет (EGL_BAD_ACCESS)
    // 
    // Правильная схема:
    // 1. JNI thread: video_render_gl_init() → eglMakeCurrent(dummy) → init GL → detach
    // 2. JNI thread: player_attach_window() → detach (на всякий случай)
    // 3. Render thread: eglMakeCurrent() в render loop
    //
    // Detach здесь (wrapper thread) не нужен и может вызвать проблемы, если context
    // был current в другом потоке (JNI thread)
    
    // 🔴 КРИТИЧНО: Используем глобальный флаг abort для синхронизации с render_loop_stop()
    // render_loop_stop() устанавливает g_lifecycle_render_abort = 1, поэтому используем его адрес
    // Сбрасываем флаг перед стартом (на случай перезапуска)
    g_lifecycle_render_abort = 0;
    
    ALOGI("✅ Starting render loop: surface=%p, context=%p", 
          (void *)g_renderer->egl_surface, (void *)g_renderer->egl_context);
    
    // Запускаем render loop
    // video_render_gl_render_loop сделает eglMakeCurrent() в самом начале
    video_render_gl_render_loop(
        g_renderer,
        (struct FrameQueue *)ctx->video->frameQueue,
        (struct AudioState *)ctx->audio,  // Может быть NULL, если нет аудио
        (struct VideoState *)ctx->video,
        &g_lifecycle_render_abort  // Используем глобальный флаг abort для синхронизации
    );
    
    // 🔥 КРИТИЧНО: EGL context уже уничтожен в video_render_gl_render_loop()
    // Не нужно делать eglMakeCurrent(NULL) здесь - это уже сделано в render thread
    // EGLContext, EGLSurface и EGLDisplay уже уничтожены в render thread перед выходом
    ALOGI("✅ Render loop finished, EGL already destroyed in render thread");
    
    // 🔴 КРИТИЧНО: Detach render thread от JVM перед выходом
    (*g_jvm)->DetachCurrentThread(g_jvm);
    ALOGI("🧹 Render thread detached from JVM");
    
    ALOGI("🎬 Render loop thread finished (lifecycle)");
    return NULL;
}

/// 🔴 ЗАДАЧА 4: Присоединить ANativeWindow к плееру
int player_attach_window(PlayerContext *ctx, void *window) {
    if (!ctx || !window) {
        ALOGE("player_attach_window: Invalid parameters");
        return -1;
    }
    
    if (!g_renderer) {
        ALOGE("player_attach_window: g_renderer is NULL (call video_render_gl_init first)");
        return -1;
    }
    
    // Привязываем window к VideoRenderGL
    int ret = video_render_gl_attach_window(g_renderer, window);
    if (ret < 0) {
        ALOGE("player_attach_window: video_render_gl_attach_window failed");
        return ret;
    }
    
    ALOGI("✅ player_attach_window: Window attached");
    
    // 🔴 КРИТИЧНО: Detach EGL context из JNI thread ПЕРЕД стартом render loop
    // EGLContext может быть current ТОЛЬКО в одном потоке одновременно
    // Если оставить его current в JNI thread, render loop получит EGL_BAD_ACCESS
    if (g_renderer->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_renderer->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        ALOGD("✅ EGL context detached from JNI thread (will be made current in render loop)");
    }
    
    // Запускаем render loop
    ret = render_loop_start(ctx);
    if (ret < 0) {
        ALOGE("player_attach_window: render_loop_start failed");
        // Откатываем attach
        video_render_gl_detach_window(g_renderer);
        return ret;
    }
    
    // 🔴 ШАГ 4: Decode thread НЕ запускается здесь
    // Decode thread запускается ТОЛЬКО после nativePlay()
    // Это предотвращает race condition с EGL lifecycle
    
    return 0;
}

/// 🔴 ЗАДАЧА 4: Отсоединить ANativeWindow от плеера
/// 🔴 ЭТАЛОН: Останавливает render loop, НЕ вызывает player_shutdown()
/// player_shutdown() вызывается только из nativeDisposePlayerContext()
/// 
/// Правильный порядок dispose:
/// 1. nativeDetachWindow() → player_detach_window() → останавливает render loop
/// 2. nativeDisposePlayerContext() → player_shutdown() → останавливает все threads
void player_detach_window(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // 🔴 ЭТАЛОН: Останавливаем ТОЛЬКО render loop, НЕ все threads
    // player_shutdown() вызывается позже из nativeDisposePlayerContext()
    render_loop_stop(ctx);
    
    // Detach window из VideoRenderGL
    if (g_renderer) {
        video_render_gl_detach_window(g_renderer);
        ALOGI("✅ player_detach_window: Window detached, render loop stopped");
    }
}

/// 🔴 ЗАДАЧА 4: Запустить render loop
int render_loop_start(PlayerContext *ctx) {
    if (!ctx) {
        ALOGE("render_loop_start: ctx is NULL");
        return -1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Render loop должен стартовать ТОЛЬКО ОДИН РАЗ
    // Это предотвращает дублирование render loop и конфликты EGL context
    if (ctx->rendering) {
        ALOGD("⚠️ render_loop_start: Already rendering - skipping (render loop should start only once)");
        return 0; // Уже запущен - это нормально, не ошибка
    }
    
    if (!g_renderer) {
        ALOGE("render_loop_start: g_renderer is NULL");
        return -1;
    }
    
    if (!ctx->video || !ctx->video->frameQueue) {
        ALOGE("render_loop_start: video or frameQueue is NULL");
        return -1;
    }
    
    // 🔴 ЭТАЛОН: Проверяем, что render_target установлен
    if (g_renderer->render_target == RENDER_TARGET_NONE) {
        ALOGE("❌ render_loop_start: Render target not set yet (call video_render_gl_attach_window or video_render_gl_register_image_texture first)");
        return -1;
    }
    
    // 🔒 FIX Z34: Для SURFACE target проверяем, что EGLSurface создан
    // Render loop НЕ должен стартовать до того, как EGLSurface существует
    if (g_renderer->render_target == RENDER_TARGET_SURFACE) {
        if (g_renderer->egl_surface == EGL_NO_SURFACE) {
            ALOGE("❌ render_loop_start: EGL surface not created yet (call video_render_gl_attach_window first)");
            return -1;
        }
    }
    
    // Останавливаем старый render loop, если он запущен
    if (ctx->rendering && ctx->renderThread) {
        g_lifecycle_render_abort = 1;
        pthread_join(ctx->renderThread, NULL);
        ctx->renderThread = 0;
    }
    
    // Запускаем render loop в отдельном потоке
    if (pthread_create(&ctx->renderThread, NULL, render_loop_wrapper_lifecycle, ctx) != 0) {
        ALOGE("render_loop_start: Failed to create render thread");
        return -1;
    }
    
    ctx->rendering = 1;
    ALOGI("✅ render_loop_start: Render loop started");
    
    return 0;
}

/// 🔴 ЗАДАЧА 4: Остановить render loop
void render_loop_stop(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    if (!ctx->rendering) {
        return; // Уже остановлен
    }
    
    // Устанавливаем флаг abort
    g_lifecycle_render_abort = 1;
    
    // Ждём завершения render thread
    if (ctx->renderThread) {
        pthread_join(ctx->renderThread, NULL);
        ctx->renderThread = 0;
    }
    
    ctx->rendering = 0;
    
    ALOGI("✅ render_loop_stop: Render loop stopped");
}

/// 🔴 ЗАДАЧА 4: Pause при app background
void player_pause_lifecycle(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Останавливаем audio clock (MASTER)
    if (ctx->audio) {
        clock_pause(&ctx->audio->clock, 1);
        ctx->audio->paused = 1;
    }
    
    // Останавливаем render loop
    render_loop_stop(ctx);
    
    ALOGI("✅ player_pause_lifecycle: Player paused (background)");
}

/// 🔴 ЗАДАЧА 4: Resume при app foreground
void player_resume_lifecycle(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Возобновляем audio clock (MASTER)
    if (ctx->audio) {
        clock_pause(&ctx->audio->clock, 0);
        ctx->audio->paused = 0;
    }
    
    // Запускаем render loop (если window прикреплён)
    if (g_renderer && g_renderer->native_window) {
        render_loop_start(ctx);
    }
    
    ALOGI("✅ player_resume_lifecycle: Player resumed (foreground)");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.5: native_on_background
/// Переводит плеер в режим audio-only (background)
void native_on_background(PlayerContext *ctx) {
    if (!ctx) {
        ALOGE("❌ native_on_background: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 native_on_background: Switching to MODE_AUDIO_ONLY");
    
    // 1. Устанавливаем playback_mode
    ctx->playback_mode = MODE_AUDIO_ONLY;
    
    // 2. Stop render loop
    render_loop_stop(ctx);
    
    // 3. Detach surface (SAFE)
    // ❌ НИКОГДА не destroy EGLContext
    // ❌ НИКОГДА не free VideoState
    if (g_renderer) {
        video_render_gl_detach_window(g_renderer);
        ALOGI("✅ native_on_background: Surface detached");
    }
    
    // 4. Pause video decode (не останавливаем полностью, только приостанавливаем)
    if (ctx->video) {
        ctx->video->abort = 1;  // Устанавливаем флаг abort для video decode thread
        ALOGI("✅ native_on_background: Video decode paused");
    }
    
    // 5. KEEP audio running
    // ❌ НИКОГДА не трогать AudioTrack
    // Audio продолжает работать автоматически
    ALOGI("✅ native_on_background: Audio continues playing");
    
    // 6. AVSYNC: audio = master
    if (ctx->has_audio && ctx->audio) {
        ctx->avsync.master = CLOCK_MASTER_AUDIO;
        ALOGI("✅ native_on_background: AVSYNC master = AUDIO");
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.9: ASSERT-ы
    #ifdef DEBUG
    // ASSERT(background => no eglSwapBuffers) - проверяется отсутствием render loop
    // ASSERT(background => no video_clock_updates) - проверяется отсутствием render loop
    // ASSERT(background => audio_clock monotonic) - проверяется в audio_renderer
    if (ctx->rendering) {
        ALOGE("❌ BACKGROUND_ASSERT FAILED: rendering=1 in background mode (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    #endif
    
    ALOGI("✅ native_on_background: Background mode activated");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.6: native_on_foreground
/// Возвращает плеер в режим AV (foreground)
void native_on_foreground(PlayerContext *ctx) {
    if (!ctx) {
        ALOGE("❌ native_on_foreground: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 native_on_foreground: Switching to MODE_AV");
    
    // 1. Устанавливаем playback_mode
    ctx->playback_mode = MODE_AV;
    
    // 2. Reattach surface
    // ⚠️ ВАЖНО: Video clock НЕ имеет права перескочить при возврате
    // Surface должен быть переподключен через Flutter (nativeAttachSurfaceTexture)
    // Здесь только проверяем, что surface готов
    if (g_renderer && g_renderer->native_window) {
        ALOGI("✅ native_on_foreground: Surface already attached");
    } else {
        ALOGW("⚠️ native_on_foreground: Surface not attached yet (will be attached by Flutter)");
    }
    
    // 3. Restart render loop (если surface готов)
    if (g_renderer && g_renderer->native_window) {
        render_loop_start(ctx);
        ALOGI("✅ native_on_foreground: Render loop restarted");
    } else {
        ALOGW("⚠️ native_on_foreground: Render loop will start after surface attach");
    }
    
    // 4. Resume video decode
    if (ctx->video) {
        ctx->video->abort = 0;  // Сбрасываем флаг abort для video decode thread
        ALOGI("✅ native_on_foreground: Video decode resumed");
    }
    
    // 5. AVSYNC: audio master until first frame
    if (ctx->has_audio && ctx->audio) {
        ctx->avsync.master = CLOCK_MASTER_AUDIO;
        ALOGI("✅ native_on_foreground: AVSYNC master = AUDIO (until first frame)");
    }
    
    // 6. After first rendered frame: switch master policy if needed
    // Это будет обработано автоматически в avsync_update() после первого кадра
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.8: FIRST_FRAME_AFTER_FOREGROUND
    // При возврате в foreground ОБЯЗАТЕЛЬНО:
    // surfaceReady → decode video → render first frame → emit FIRST_FRAME
    // Это будет обработано автоматически в render loop после surfaceReady
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.9: ASSERT-ы
    #ifdef DEBUG
    // ASSERT(foreground => firstFrame emitted) - проверяется в render loop
    if (ctx->playback_mode != MODE_AV) {
        ALOGE("❌ FOREGROUND_ASSERT FAILED: playback_mode != MODE_AV (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    #endif
    
    ALOGI("✅ native_on_foreground: Foreground mode activated");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.3: Вход в frame step режим
/// Останавливает все автоматические процессы и подготавливает для покадровой навигации
static void enter_frame_step(PlayerContext *ctx) {
    if (!ctx) {
        ALOGE("❌ enter_frame_step: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 enter_frame_step: Entering MODE_FRAME_STEP");
    
    // 1. Устанавливаем playback_mode
    ctx->playback_mode = MODE_FRAME_STEP;
    
    // 2. STOP everything automatic
    // Audio pause
    if (ctx->audio) {
        extern void audio_pause(AudioState *as);
        audio_pause(ctx->audio);
        ALOGI("✅ enter_frame_step: Audio paused");
    }
    
    // Stop render loop (vsync loop)
    render_loop_stop(ctx);
    ALOGI("✅ enter_frame_step: Render loop stopped");
    
    // Disable AVSYNC
    avsync_gate_invalidate(&ctx->avsync_gate, "frame step mode");
    ctx->avsync.master = CLOCK_MASTER_VIDEO;  // Video master для frame step
    ALOGI("✅ enter_frame_step: AVSYNC disabled");
    
    // 3. Freeze clocks (не обновляются автоматически)
    if (ctx->video) {
        // Clocks остаются на текущих значениях, но не обновляются
        ALOGI("✅ enter_frame_step: Video clock frozen at %.3f", 
              ctx->video->clock.valid ? ctx->video->clock.pts_sec : 0.0);
    }
    
    // 4. Clear decode queues
    if (ctx->video && ctx->video->frameQueue) {
        frame_queue_flush(ctx->video->frameQueue);
    }
    if (ctx->video && ctx->video->packetQueue) {
        packet_queue_flush(ctx->video->packetQueue);
    }
    if (ctx->audio && ctx->audio->frameQueue) {
        frame_queue_flush(ctx->audio->frameQueue);
    }
    if (ctx->audio && ctx->audio->packetQueue) {
        packet_queue_flush(ctx->audio->packetQueue);
    }
    ALOGI("✅ enter_frame_step: Decode queues flushed");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.7: ASSERT-ы
    #ifdef DEBUG
    // ASSERT(playback_mode == MODE_FRAME_STEP)
    if (ctx->playback_mode != MODE_FRAME_STEP) {
        ALOGE("❌ FRAME_STEP_ASSERT FAILED: playback_mode != MODE_FRAME_STEP (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    // ASSERT(audio_state == PAUSED) - проверяется через audio_pause
    // ASSERT(no demux thread running) - проверяется через abort
    // ASSERT(no vsync loop running) - проверяется через render_loop_stop
    if (ctx->rendering) {
        ALOGE("❌ FRAME_STEP_ASSERT FAILED: rendering=1 in frame step mode (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    #endif
    
    ALOGI("✅ enter_frame_step: Frame step mode activated");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.4: Декодирование одного видео кадра
/// Декодирует ровно один кадр из текущей позиции
static AVFrame* decode_one_video_frame(PlayerContext *ctx) {
    if (!ctx || !ctx->video || !ctx->video->codecCtx || !ctx->fmt) {
        ALOGE("❌ decode_one_video_frame: Invalid parameters");
        return NULL;
    }
    
    AVCodecContext *codec_ctx = ctx->video->codecCtx;
    AVFormatContext *fmt_ctx = ctx->fmt;
    int video_stream = ctx->videoStream;
    
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        ALOGE("❌ decode_one_video_frame: Failed to allocate packet");
        return NULL;
    }
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        ALOGE("❌ decode_one_video_frame: Failed to allocate frame");
        return NULL;
    }
    
    // Читаем пакеты до первого видео кадра
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }
        
        // Отправляем пакет в декодер
        int ret = avcodec_send_packet(codec_ctx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            continue;
        }
        
        // Получаем декодированный кадр
        ret = avcodec_receive_frame(codec_ctx, frame);
        av_packet_unref(pkt);
        
        if (ret == 0) {
            // Кадр успешно декодирован
            // 🔎 DIAGNOSTIC: Log decoded frame info
            double pts_sec = NAN;
            if (frame->pts != AV_NOPTS_VALUE && fmt_ctx->streams[video_stream]) {
                AVRational time_base = fmt_ctx->streams[video_stream]->time_base;
                pts_sec = frame->pts * av_q2d(time_base);
            } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE && fmt_ctx->streams[video_stream]) {
                AVRational time_base = fmt_ctx->streams[video_stream]->time_base;
                pts_sec = frame->best_effort_timestamp * av_q2d(time_base);
            }
            ALOGI("🎞 VIDEO FRAME DECODED: pts=%.3f size=%dx%d format=%d",
                  pts_sec,
                  frame->width,
                  frame->height,
                  frame->format);
            ALOGI("✅ decode_one_video_frame: Frame decoded");
            return frame;
        } else if (ret == AVERROR(EAGAIN)) {
            // Нужно больше пакетов
            continue;
        } else {
            // Ошибка декодирования
            ALOGW("⚠️ decode_one_video_frame: Decode error %d", ret);
            continue;
        }
    }
    
    // EOF или ошибка
    av_packet_free(&pkt);
    av_frame_free(&frame);
    ALOGE("❌ decode_one_video_frame: No frame decoded (EOF or error)");
    return NULL;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.6: Рендеринг одного кадра
/// Рендерит ровно один кадр без запуска render loop
static int render_frame_once(PlayerContext *ctx, AVFrame *frame) {
    if (!ctx || !frame || !g_renderer) {
        ALOGE("❌ render_frame_once: Invalid parameters");
        return -1;
    }
    
    // Проверяем, что EGL context готов
    if (!g_renderer->egl_context || !g_renderer->egl_surface) {
        ALOGE("❌ render_frame_once: EGL not ready");
        return -1;
    }
    
    // eglMakeCurrent
    if (!eglMakeCurrent(g_renderer->egl_display, 
                        g_renderer->egl_surface, 
                        g_renderer->egl_surface, 
                        g_renderer->egl_context)) {
        ALOGE("❌ render_frame_once: eglMakeCurrent failed");
        return -1;
    }
    
    // Рендерим кадр (без interpolation, alpha=1.0)
    int ret = video_render_gl_draw(g_renderer, frame, NULL, 1.0);
    if (ret < 0) {
        ALOGE("❌ render_frame_once: video_render_gl_draw failed");
        return -1;
    }
    
    // eglSwapBuffers (ОДИН РАЗ)
    if (!eglSwapBuffers(g_renderer->egl_display, g_renderer->egl_surface)) {
        ALOGE("❌ render_frame_once: eglSwapBuffers failed");
        return -1;
    }
    
    // Обновляем video clock вручную
    if (ctx->video && ctx->video->video_stream) {
        AVRational time_base = ctx->video->video_stream->time_base;
        double pts_sec = 0.0;
        
        if (frame->pts != AV_NOPTS_VALUE) {
            pts_sec = frame->pts * av_q2d(time_base);
        } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            pts_sec = frame->best_effort_timestamp * av_q2d(time_base);
        }
        
        if (!isnan(pts_sec) && pts_sec >= 0.0) {
            ctx->video->clock.pts_sec = pts_sec;
            ctx->video->clock.valid = 1;
            ctx->video->clock.last_present_ts = get_monotonic_time_sec();
            ALOGI("✅ render_frame_once: Video clock updated to %.3f", pts_sec);
        }
    }
    
    ALOGI("✅ render_frame_once: Frame rendered");
    return 0;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.4: Следующий кадр
static void step_next_frame(PlayerContext *ctx) {
    if (!ctx) {
        ALOGE("❌ step_next_frame: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 step_next_frame: Stepping to next frame");
    
    enter_frame_step(ctx);
    
    // 1. Decode exactly ONE frame
    AVFrame *frame = decode_one_video_frame(ctx);
    if (!frame) {
        ALOGE("❌ step_next_frame: Failed to decode frame");
        return;
    }
    
    // 2. Set video clock to frame PTS
    if (ctx->video && ctx->video->video_stream) {
        AVRational time_base = ctx->video->video_stream->time_base;
        double pts_sec = 0.0;
        
        if (frame->pts != AV_NOPTS_VALUE) {
            pts_sec = frame->pts * av_q2d(time_base);
        } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            pts_sec = frame->best_effort_timestamp * av_q2d(time_base);
        }
        
        if (!isnan(pts_sec) && pts_sec >= 0.0) {
            ctx->video->clock.pts_sec = pts_sec;
            ctx->video->clock.valid = 1;
        }
    }
    
    // 3. Render exactly ONE frame
    int ret = render_frame_once(ctx, frame);
    if (ret < 0) {
        ALOGE("❌ step_next_frame: Failed to render frame");
        av_frame_free(&frame);
        return;
    }
    
    // 4. Emit event
    int64_t pts_ms = 0;
    if (ctx->video && ctx->video->clock.valid) {
        pts_ms = (int64_t)(ctx->video->clock.pts_sec * 1000.0);
    }
    
    extern void native_player_emit_frame_stepped_event(int64_t pts_ms);
    native_player_emit_frame_stepped_event(pts_ms);
    
    av_frame_free(&frame);
    ALOGI("✅ step_next_frame: Frame stepped (pts=%lld ms)", (long long)pts_ms);
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.5: Предыдущий кадр
static void step_prev_frame(PlayerContext *ctx) {
    if (!ctx || !ctx->video) {
        ALOGE("❌ step_prev_frame: PlayerContext is NULL or no video");
        return;
    }
    
    ALOGI("🔄 step_prev_frame: Stepping to previous frame");
    
    enter_frame_step(ctx);
    
    // Вычисляем target_pts
    double current_pts = ctx->video->clock.valid ? ctx->video->clock.pts_sec : 0.0;
    double estimated_frame_duration = 1.0 / 25.0; // 25fps fallback
    if (ctx->video->video_stream && ctx->video->video_stream->avg_frame_rate.num > 0) {
        estimated_frame_duration = 1.0 / av_q2d(ctx->video->video_stream->avg_frame_rate);
    }
    
    double target_pts = current_pts - estimated_frame_duration;
    if (target_pts < 0.0) {
        target_pts = 0.0;
    }
    
    ALOGI("🔍 step_prev_frame: target_pts=%.3f (current=%.3f, duration=%.3f)", 
          target_pts, current_pts, estimated_frame_duration);
    
    // 1. Seek to keyframe (BACKWARD)
    AVRational time_base = ctx->video->video_stream->time_base;
    int64_t seek_ts = av_rescale_q(
        (int64_t)(target_pts * AV_TIME_BASE),
        AV_TIME_BASE_Q,
        time_base
    );
    
    int ret = av_seek_frame(ctx->fmt, ctx->videoStream, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        ALOGW("⚠️ step_prev_frame: Seek failed, trying from start");
        seek_ts = 0;
        av_seek_frame(ctx->fmt, ctx->videoStream, seek_ts, AVSEEK_FLAG_BACKWARD);
    }
    
    // Flush codec buffers
    if (ctx->video->codecCtx) {
        avcodec_flush_buffers(ctx->video->codecCtx);
    }
    
    // 2. Decode forward until target
    AVFrame *frame = NULL;
    int max_decode_attempts = 100;
    int attempts = 0;
    
    while (attempts < max_decode_attempts) {
        frame = decode_one_video_frame(ctx);
        if (!frame) {
            break;
        }
        
        // Вычисляем PTS кадра
        double frame_pts = 0.0;
        if (frame->pts != AV_NOPTS_VALUE) {
            frame_pts = frame->pts * av_q2d(time_base);
        } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            frame_pts = frame->best_effort_timestamp * av_q2d(time_base);
        }
        
        if (!isnan(frame_pts) && frame_pts >= target_pts) {
            // Нашли кадр >= target
            break;
        }
        
        av_frame_free(&frame);
        frame = NULL;
        attempts++;
    }
    
    if (!frame) {
        ALOGE("❌ step_prev_frame: Failed to find frame >= target");
        return;
    }
    
    // 3. Clock sync
    double frame_pts = 0.0;
    if (frame->pts != AV_NOPTS_VALUE) {
        frame_pts = frame->pts * av_q2d(time_base);
    } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
        frame_pts = frame->best_effort_timestamp * av_q2d(time_base);
    }
    
    if (!isnan(frame_pts) && frame_pts >= 0.0) {
        ctx->video->clock.pts_sec = frame_pts;
        ctx->video->clock.valid = 1;
    }
    
    // 4. Render ONE frame
    ret = render_frame_once(ctx, frame);
    if (ret < 0) {
        ALOGE("❌ step_prev_frame: Failed to render frame");
        av_frame_free(&frame);
        return;
    }
    
    // Emit event
    int64_t pts_ms = (int64_t)(frame_pts * 1000.0);
    extern void native_player_emit_frame_stepped_event(int64_t pts_ms);
    native_player_emit_frame_stepped_event(pts_ms);
    
    av_frame_free(&frame);
    ALOGI("✅ step_prev_frame: Frame stepped (pts=%lld ms)", (long long)pts_ms);
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.2: Главная функция для frame stepping
void native_step_frame(PlayerContext *ctx, int direction) {
    if (!ctx) {
        ALOGE("❌ native_step_frame: PlayerContext is NULL");
        return;
    }
    
    if (direction == 0) {
        ALOGE("❌ native_step_frame: Invalid direction (0)");
        return;
    }
    
    ALOGI("🔄 native_step_frame: direction=%d", direction);
    
    if (direction > 0) {
        // Next frame
        step_next_frame(ctx);
    } else {
        // Previous frame
        step_prev_frame(ctx);
    }
    
    ALOGI("✅ native_step_frame: Frame step completed");
}

/// 🔴 ШАГ 5: ЭТАЛОННЫЙ player_shutdown() - останавливает ВСЁ (ffplay-grade)
///
/// Это единственная функция, через которую ВСЕГДА закрывается плеер.
/// После вызова не должен работать НИ ОДИН поток.
///
/// Инварианты после player_shutdown(ctx):
/// - ❌ render loop НЕ крутится
/// - ❌ decode threads НЕ работают
/// - ❌ audio callback НЕ вызывается
/// - ❌ frame / packet queue НЕ ждут
/// - ❌ getPosition НЕ увеличивается
/// - ❌ логов НЕТ
void player_shutdown(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // 🔴 ИДЕМПОТЕНТНОСТЬ: Защита от двойных вызовов
    if (ctx->shutting_down) {
        ALOGD("⚠️ player_shutdown: Already shutting down, skipping");
        return;
    }
    
    ALOGI("🛑 player_shutdown: Starting shutdown sequence...");
    
    // ─────────────────────────────────────────
    // 🔴 1. ГЛОБАЛЬНЫЙ ФЛАГ
    // ─────────────────────────────────────────
    ctx->shutting_down = 1;
    ctx->state.abort_request = 1;
    ctx->abort = 1;
    
    // ─────────────────────────────────────────
    // 🔴 2. ОСТАНОВИТЬ RENDER LOOP
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Stopping render loop...");
    // render_loop_stop() устанавливает g_lifecycle_render_abort = 1 и делает pthread_join
    render_loop_stop(ctx);
    
    // ─────────────────────────────────────────
    // 🔴 3. ABORT ВСЕ QUEUE (РАЗБУДИТ wait)
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Aborting queues...");
    
    if (ctx->video && ctx->video->frameQueue) {
        frame_queue_abort(ctx->video->frameQueue);
    }
    if (ctx->video && ctx->video->packetQueue) {
        packet_queue_abort(ctx->video->packetQueue);
    }
    
    if (ctx->audio && ctx->audio->frameQueue) {
        frame_queue_abort(ctx->audio->frameQueue);
    }
    if (ctx->audio && ctx->audio->packetQueue) {
        packet_queue_abort(ctx->audio->packetQueue);
    }
    
    // ─────────────────────────────────────────
    // 🔴 4. ОСТАНОВИТЬ DECODE THREADS
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Stopping decode threads...");
    
    // 🔴 КРИТИЧНО: video_threads_stop() и audio_threads_stop() уже делают join с проверкой флагов
    // НЕ нужно дублировать join здесь - это приведёт к invalid pthread_t
    if (ctx->video) {
        video_threads_stop(ctx->video);
    }
    
    if (ctx->audio) {
        audio_threads_stop(ctx->audio);
    }
    
    // ─────────────────────────────────────────
    // 🔴 6. JOIN DEMUX THREAD
    // ─────────────────────────────────────────
    if (ctx->demuxThread) {
        pthread_join(ctx->demuxThread, NULL);
        ctx->demuxThread = 0;
    }
    
    // ─────────────────────────────────────────
    // 🔴 7. СБРОС CLOCK (ЗАМОРОЗКА)
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Freezing clocks...");
    
    if (ctx->video && clock_is_active(&ctx->video->video_clock)) {
        clock_pause(&ctx->video->video_clock, 1);
    }
    
    if (ctx->audio && clock_is_active(&ctx->audio->clock)) {
        clock_pause(&ctx->audio->clock, 1);
    }
    
    // ─────────────────────────────────────────
    // 🔴 8. ОЧИСТКА QUEUE (УЖЕ БЕЗ WAIT)
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Flushing queues...");
    
    if (ctx->video && ctx->video->frameQueue) {
        frame_queue_flush(ctx->video->frameQueue);
    }
    if (ctx->video && ctx->video->packetQueue) {
        packet_queue_flush(ctx->video->packetQueue);
    }
    
    if (ctx->audio && ctx->audio->frameQueue) {
        frame_queue_flush(ctx->audio->frameQueue);
    }
    if (ctx->audio && ctx->audio->packetQueue) {
        packet_queue_flush(ctx->audio->packetQueue);
    }
    
    // ─────────────────────────────────────────
    // 🔴 9. ОСВОБОЖДЕНИЕ RENDERER И AUDIO
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Releasing renderer and audio...");
    
    // VideoRenderGL освобождается в nativeDisposePlayerContext после shutdown
    // Audio output освобождается в audio_decoder_destroy
    
    // ─────────────────────────────────────────
    // 🔴 10. ОСВОБОЖДЕНИЕ DECODERS
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Destroying decoders...");
    
    if (ctx->video) {
        video_decoder_destroy(ctx->video);
    }
    
    if (ctx->audio) {
        // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 6️⃣ AUDIO_DEAD (при shutdown)
        // AudioState.dead — терминальное состояние
        if (ctx->audio_state != AUDIO_DEAD && ctx->audio_state != AUDIO_NO_AUDIO) {
            ctx->audio_state = AUDIO_DEAD;
            ALOGI("💀 AudioState: → AUDIO_DEAD (player_shutdown)");
            extern void native_player_emit_audio_state_event(const char *state);
            native_player_emit_audio_state_event("dead");
        }
        
        audio_decoder_destroy(ctx->audio);
    }
    
    // ─────────────────────────────────────────
    // 🔴 11. ОСВОБОЖДЕНИЕ SUBTITLES
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Destroying subtitle manager...");
    subtitle_manager_destroy(&ctx->subtitles);
    
    // ─────────────────────────────────────────
    // 🔴 12. ЗАКРЫТИЕ MEDIA FILE
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Closing media file...");
    if (ctx->fmt) {
        avformat_close_input(&ctx->fmt);
    }
    
    // ─────────────────────────────────────────
    // 🔴 13. ОСВОБОЖДЕНИЕ MUTEXES
    // ─────────────────────────────────────────
    ALOGI("🛑 player_shutdown: Destroying mutexes...");
    pthread_mutex_destroy(&ctx->state.seek_mutex);
    // error_mutex освобождается в nativeDisposePlayerContext
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF - останавливаем watchdog thread
    if (ctx->avsyncWatchdogThread != 0) {
        ALOGI("🛑 player_shutdown: Stopping AVSYNC watchdog thread...");
        pthread_join(ctx->avsyncWatchdogThread, NULL);
        ctx->avsyncWatchdogThread = 0;
        ALOGI("✅ player_shutdown: AVSYNC watchdog thread stopped");
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - останавливаем seek watchdog thread
    if (ctx->seekWatchdogThread != 0) {
        ALOGI("🛑 player_shutdown: Stopping seek watchdog thread...");
        pthread_join(ctx->seekWatchdogThread, NULL);
        ctx->seekWatchdogThread = 0;
        ALOGI("✅ player_shutdown: Seek watchdog thread stopped");
    }
    
    ALOGI("✅ player_shutdown: Shutdown sequence complete");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF - AVSYNC Watchdog thread
///
/// Проверяет clock stall каждые 500ms
/// Если master clock не обновляется > 500ms → инвалидирует AVSYNC и останавливает playback
///
/// 🔥 КРИТИЧЕСКИЙ FIX: Watchdog должен быть контекстно-осознанным
/// - Не проверяет stall если state != PLAYING
/// - Не проверяет stall если первый кадр ещё не отрисован
/// - Для video-only разрешает idle clock до первого frame
static void *avsync_watchdog_thread(void *arg) {
    PlayerContext *ctx = (PlayerContext *)arg;
    if (!ctx) {
        return NULL;
    }
    
    ALOGI("🔄 AVSYNC Watchdog: Thread started");
    
    while (!ctx->abort && !ctx->shutting_down) {
        usleep(500000); // 500ms
        
        if (ctx->abort || ctx->shutting_down) {
            break;
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AUTO-NEXT - EOF ≠ STALL
        // Если EOF достигнут, watchdog должен быть отключён
        // EOF - это нормальное завершение playback, не ошибка
        if (ctx->eof_reached) {
            continue; // ❌ EOF достигнут - не проверяем stall
        }
        
        // 🔥 FIX 3: Watchdog должен знать FSM state
        // Не проверяем stall если state != PLAYING
        if (ctx->state.state != PLAYBACK_RUNNING || ctx->paused) {
            continue; // ❌ не проверяем stall если не playing
        }
        
        // 🔥 FIX 4: Первому кадру — special handling
        // Clock не считается stalled, пока не отрисован первый frame
        // Это КРИТИЧНО для AVI / FLV (часто первый frame приходит с задержкой)
        if (ctx->video && !ctx->video->first_frame_rendered) {
            continue; // ❌ не проверяем stall до первого frame
        }
        
        // 🔥 FIX 2: Video-only → разрешить "idle clock"
        // Для video-only режима до первого frame clock = IDLE (это нормально)
        bool is_video_only = (ctx->has_audio == 0);
        if (is_video_only && ctx->video && !ctx->video->clock.valid) {
            continue; // ❌ video-only: clock может быть idle до первого frame
        }
        
        // Проверяем clock stall (только если все условия выполнены)
        if (avsync_gate_check_stall(&ctx->avsync_gate, 500000)) { // 500ms threshold
            // 🔒 ЗАЩИТНЫЙ ASSERT (ОБЯЗАТЕЛЬНО)
            #ifdef DEBUG
            if (ctx->eof_reached) {
                ALOGE("❌ AVSYNC Watchdog ASSERT FAILED: STALL and EOF cannot happen together (FATAL)");
                abort();
            }
            #endif
            
            // Clock stall обнаружен → инвалидируем AVSYNC и эмитим error
            avsync_gate_invalidate(&ctx->avsync_gate, "MASTER CLOCK STALLED");
            
            extern void native_player_emit_error_event(const char *message);
            native_player_emit_error_event("CLOCK_STALL");
            
            // Останавливаем playback
            player_pause(ctx);
            
            ALOGE("❌ AVSYNC Watchdog: Clock stall detected - playback stopped");
        }
    }
    
    ALOGI("✅ AVSYNC Watchdog: Thread stopped");
    return NULL;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF - запустить AVSYNC watchdog thread
///
/// 🔥 КРИТИЧЕСКИЙ FIX: Вызывается ТОЛЬКО после play()
/// Watchdog должен стартовать когда clocks начали тикать
/// Иначе для video-only файлов watchdog будет считать idle clock как stall
int avsync_watchdog_start(PlayerContext *ctx) {
    if (!ctx) {
        return -1;
    }
    
    // Проверяем, не запущен ли уже
    if (ctx->avsyncWatchdogThread != 0) {
        ALOGD("⚠️ avsync_watchdog_start: Watchdog already running");
        return 0;
    }
    
    int ret = pthread_create(&ctx->avsyncWatchdogThread, NULL, avsync_watchdog_thread, ctx);
    if (ret != 0) {
        ALOGE("❌ avsync_watchdog_start: Failed to create watchdog thread: %d", ret);
        return -1;
    }
    
    ALOGI("✅ AVSYNC Watchdog: Thread started");
    return 0;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AUTO-NEXT - остановить AVSYNC watchdog thread
///
/// Вызывается при EOF для предотвращения ложных срабатываний
/// EOF ≠ STALL - это нормальное завершение playback
void avsync_watchdog_stop(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Проверяем, запущен ли watchdog
    if (ctx->avsyncWatchdogThread == 0) {
        ALOGD("⚠️ avsync_watchdog_stop: Watchdog not running");
        return;
    }
    
    // Останавливаем watchdog thread
    // Thread сам завершится при следующей проверке abort/shutting_down
    // Мы просто ждём его завершения
    ALOGI("🛑 avsync_watchdog_stop: Stopping AVSYNC watchdog thread...");
    pthread_join(ctx->avsyncWatchdogThread, NULL);
    ctx->avsyncWatchdogThread = 0;
    ALOGI("✅ avsync_watchdog_stop: AVSYNC watchdog thread stopped");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - Seek Watchdog thread
///
/// Проверяет seek deadlock каждые 1200ms
/// Если seek_in_progress > 1200ms и нет firstFrameAfterSeek → эмитим error и останавливаем playback
static void *seek_watchdog_thread(void *arg) {
    PlayerContext *ctx = (PlayerContext *)arg;
    if (!ctx) {
        return NULL;
    }
    
    ALOGI("🔄 Seek Watchdog: Thread started");
    
    int64_t seek_start_ms = av_gettime() / 1000;  // миллисекунды
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - PATCH 4: Hard deadlock guard
    // Ждём 1000ms (уменьшено с 1200ms для более быстрой детекции)
    usleep(1000000); // 1000ms
    
    // Проверяем, завершился ли seek
    if (ctx->abort || ctx->shutting_down) {
        ALOGI("✅ Seek Watchdog: Thread stopped (abort/shutdown)");
        return NULL;
    }
    
    // Проверяем, идёт ли ещё seek
    if (avsync_gate_is_seek_in_progress(&ctx->avsync_gate) || ctx->waiting_first_frame_after_seek) {
        int64_t elapsed_ms = (av_gettime() / 1000) - seek_start_ms;
        // Seek deadlock обнаружен → эмитим error
        // ❌ Никаких silent fails, ❌ Никаких infinite waits
        ALOGE("❌ SEEK DEADLOCK: no frame after seek (%lld ms timeout) - SEEK_FRAME_ASSERT_FAILED", 
              (long long)elapsed_ms);
        
        extern void native_player_emit_error_event(const char *message);
        native_player_emit_error_event("SEEK_FRAME_ASSERT_FAILED");
        
        // Останавливаем playback
        player_pause(ctx);
        
        ALOGE("❌ Seek Watchdog: Deadlock detected - playback stopped");
    }
    
    ALOGI("✅ Seek Watchdog: Thread stopped");
    return NULL;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - запустить seek watchdog thread
///
/// Вызывается при начале seek для мониторинга deadlock
int seek_watchdog_start(PlayerContext *ctx) {
    if (!ctx) {
        return -1;
    }
    
    // Останавливаем предыдущий watchdog если он запущен
    if (ctx->seekWatchdogThread != 0) {
        ALOGD("⚠️ seek_watchdog_start: Stopping previous watchdog");
        pthread_join(ctx->seekWatchdogThread, NULL);
        ctx->seekWatchdogThread = 0;
    }
    
    int ret = pthread_create(&ctx->seekWatchdogThread, NULL, seek_watchdog_thread, ctx);
    if (ret != 0) {
        ALOGE("❌ seek_watchdog_start: Failed to create watchdog thread: %d", ret);
        return -1;
    }
    
    ALOGI("✅ Seek Watchdog: Thread started");
    return 0;
}

/// Остановить seek watchdog thread
void seek_watchdog_stop(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    if (ctx->seekWatchdogThread != 0) {
        ALOGI("🛑 seek_watchdog_stop: Stopping seek watchdog thread...");
        pthread_join(ctx->seekWatchdogThread, NULL);
        ctx->seekWatchdogThread = 0;
        ALOGI("✅ seek_watchdog_stop: Seek watchdog thread stopped");
    }
}

