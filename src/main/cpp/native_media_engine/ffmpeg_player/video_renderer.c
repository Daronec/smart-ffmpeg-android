#include "video_renderer.h"
#include "ffmpeg_player.h"
#include "libavutil/time.h"
#include "libavutil/rational.h"
#include "libavutil/frame.h"
#include "packet_queue.h"
#include "frame_queue.h"
#include <math.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <android/log.h>

#define LOG_TAG "VideoRenderer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Получить monotonic time в секундах
static double get_monotonic_time_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;  // микросекунды → секунды
}

/// Проверить video stall
///
/// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17
/// 🔥 Video stall detection (ASSERT)
///
/// @param c VideoClock
/// @return 1 если stalled, 0 если running
int video_clock_is_stalled(VideoClock *c) {
    if (!c || !c->valid) {
        return 1;
    }
    
    double dt = get_monotonic_time_sec() - c->last_present_ts;
    return dt > 0.7; // 700ms
}

/// Проверить ASSERT-ы для video clock (обязательные)
///
/// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.9
/// 🧪 ASSERT (ОБЯЗАТЕЛЬНЫ)
/// ASSERT(!isnan(video_clock))
/// ASSERT(video_clock >= 0)
/// ASSERT(video_clock monotonic)
///
/// @param vs Состояние видео
/// @param ctx PlayerContext
void video_clock_assert(VideoState *vs, void *ctx_ptr) {
    if (!vs || !ctx_ptr) {
        return;
    }
    
    PlayerContext *ctx = (PlayerContext *)ctx_ptr;
    
    #ifdef DEBUG
    // 1. ASSERT(!isnan(video_clock))
    if (vs->clock.valid && isnan(vs->clock.pts_sec)) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock is NAN (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    
    // 2. ASSERT(video_clock >= 0)
    if (vs->clock.valid && vs->clock.pts_sec < 0.0) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock < 0 (%.3f) (FATAL)", vs->clock.pts_sec);
        abort(); // 🔥 FATAL в debug
    }
    
    // 3. ASSERT(video_clock monotonic)
    static double last_video_clock = 0.0;
    if (vs->clock.valid && !isnan(vs->clock.pts_sec) && vs->clock.pts_sec < last_video_clock - 0.001) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock regression (%.3f < %.3f) (FATAL)", 
              vs->clock.pts_sec, last_video_clock);
        abort(); // 🔥 FATAL в debug
    }
    if (vs->clock.valid && !isnan(vs->clock.pts_sec)) {
        last_video_clock = vs->clock.pts_sec;
    }
    
    // 4. ASSERT(video_clock.valid => frame_presented)
    if (vs->clock.valid && vs->clock.last_present_ts == 0.0) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock.valid=1 but last_present_ts=0 (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    
    // 5. ASSERT(!(video_clock > audio_clock + 0.5))
    if (vs->clock.valid && !isnan(vs->clock.pts_sec) && ctx->audio && ctx->audio->clock.valid && !isnan(ctx->audio->clock.clock)) {
        double video_clock = vs->clock.pts_sec;
        double audio_clock = ctx->audio->clock.clock;
        if (video_clock > audio_clock + 0.5) {
            ALOGE("❌ AVSYNC_ASSERT FAILED: video_clock=%.3f > audio_clock=%.3f + 0.5 (FATAL)", 
                  video_clock, audio_clock);
            abort(); // 🔥 FATAL в debug
        }
    }
    #endif
}

/// Сбросить video clock (для seek)
///
/// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.6
/// При seek:
///   - clock = NAN
///   - last_pts = NAN
///   - has_frame = 0
///   - serial++
///
/// @param vs Состояние видео
void video_clock_reset(VideoState *vs) {
    if (!vs) {
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.6: SEEK FIX
    vs->clock.pts_sec = NAN;
    vs->clock.valid = 0;
    vs->clock.last_present_ts = 0.0;
    vs->last_pts = NAN;
    vs->has_frame = 0;
    vs->serial++;
    
    // Legacy поля (deprecated)
    vs->video_clock_pts = NAN;
    vs->last_video_clock_pts = NAN;
    
    ALOGI("🔍 SEEK: video_clock reset (clock=NAN, last_pts=NAN, has_frame=0, serial=%ld)", (long)vs->serial);
}

/// Обновить video clock после eglSwapBuffers (ЕДИНСТВЕННОЕ МЕСТО)
///
/// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.3
/// ❗ ТОЛЬКО ПОСЛЕ eglSwapBuffers()
/// ❗ НЕ при decode
/// ❗ НЕ при enqueue
/// ❗ НЕ при vsync
///
/// @param vs Состояние видео
/// @param frame Кадр, который был отрисован
void video_clock_on_frame_render(VideoState *vs, AVFrame *frame) {
    if (!vs || !frame) {
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.4: BROKEN / MISSING PTS
    // Политика (строгая):
    //   - pts == NOPTS → ❌ DROP frame (не обновляем clock)
    //   - pts < last_pts → ❌ DROP (не обновляем clock)
    //   - pts jumps backwards → ❌ DROP (не обновляем clock)
    //   - pts jumps > +1s → ❌ DROP (не обновляем clock)
    
    // Получаем PTS кадра
    double pts = NAN;
    if (vs->video_stream) {
        AVRational time_base = vs->video_stream->time_base;
        if (frame->pts != AV_NOPTS_VALUE) {
            pts = (double)frame->pts * av_q2d(time_base);
        } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            pts = (double)frame->best_effort_timestamp * av_q2d(time_base);
        }
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.4: BROKEN / MISSING PTS
    // Если pts == NOPTS → не обновляем clock (кадр должен быть отброшен до этого)
    if (isnan(pts) || pts < 0.0) {
        ALOGW("⚠️ VIDEO_CLOCK: frame has no valid PTS (pts=%f), skipping clock update", pts);
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.4: BROKEN / MISSING PTS
    // Если pts < last_pts → не обновляем clock (кадр должен быть отброшен до этого)
    if (!isnan(vs->last_pts) && pts < vs->last_pts - 0.001) {
        ALOGW("⚠️ VIDEO_CLOCK: frame PTS backward (pts=%.3f < last=%.3f), skipping clock update", 
              pts, vs->last_pts);
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.4: BROKEN / MISSING PTS
    // Если pts jumps > +1s → не обновляем clock (кадр должен быть отброшен до этого)
    if (!isnan(vs->last_pts) && pts > vs->last_pts + 1.0) {
        ALOGW("⚠️ VIDEO_CLOCK: frame PTS jump forward > 1s (pts=%.3f > last=%.3f + 1.0), skipping clock update", 
              pts, vs->last_pts);
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.3: ГДЕ ОБНОВЛЯЕТСЯ VIDEO CLOCK
    // Обновляем clock ТОЛЬКО если PTS валиден и монотонен
    vs->last_pts = pts;
    vs->clock.pts_sec = pts;
    vs->clock.valid = 1;
    vs->clock.last_present_ts = get_monotonic_time_sec();
    vs->has_frame = 1;
    
    // Legacy поля (deprecated)
    vs->video_clock_pts = pts;
    vs->last_video_clock_pts = pts;
    
    // 🔍 ИНСТРУМЕНТАЦИЯ: логируем первые 10 обновлений
    static int log_count = 0;
    if (log_count < 10) {
        ALOGD("🎞 VideoClock: pts_sec=%.3f (PTS-based, after eglSwapBuffers)", vs->clock.pts_sec);
        log_count++;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.9: ASSERT
    #ifdef DEBUG
    // ASSERT(!isnan(video_clock))
    if (isnan(vs->clock.pts_sec)) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock is NAN (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    
    // ASSERT(video_clock >= 0)
    if (vs->clock.pts_sec < 0.0) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock < 0 (%.3f) (FATAL)", vs->clock.pts_sec);
        abort(); // 🔥 FATAL в debug
    }
    
    // ASSERT(video_clock monotonic)
    static double last_video_clock = 0.0;
    if (vs->clock.pts_sec < last_video_clock - 0.001) {
        ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock regression (%.3f < %.3f) (FATAL)", 
              vs->clock.pts_sec, last_video_clock);
        abort(); // 🔥 FATAL в debug
    }
    last_video_clock = vs->clock.pts_sec;
    #endif
}

/// Получить текущий video clock
///
/// @param vs Состояние видео
/// @return Текущий video clock в секундах, или NAN если невалиден
double video_get_clock(VideoState *vs) {
    if (!vs || !vs->clock.valid) {
        return NAN;
    }
    
    return vs->clock.pts_sec;
}

/// Инициализировать видео декодер
///
/// @param vs Состояние видео
/// @param stream Видео стрим из AVFormatContext
/// @return 0 при успехе, <0 при ошибке
int video_decoder_init(VideoState *vs, AVStream *stream) {
    if (!vs || !stream) {
        ALOGE("❌ video_decoder_init: Invalid parameters");
        return -1;
    }
    
    // Находим декодер
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        ALOGE("❌ video_decoder_init: Codec not found (codec_id=%d)", stream->codecpar->codec_id);
        return -1;
    }
    
    ALOGI("🎬 Video decoder found: %s", codec->name);
    
    // Выделяем codec context
    vs->codecCtx = avcodec_alloc_context3(codec);
    if (!vs->codecCtx) {
        ALOGE("❌ video_decoder_init: Failed to allocate codec context");
        return -1;
    }
    
    // Копируем параметры из стрима
    if (avcodec_parameters_to_context(vs->codecCtx, stream->codecpar) < 0) {
        ALOGE("❌ video_decoder_init: Failed to copy codec parameters");
        avcodec_free_context(&vs->codecCtx);
        return -1;
    }
    
    // Открываем декодер
    if (avcodec_open2(vs->codecCtx, codec, NULL) < 0) {
        ALOGE("❌ video_decoder_init: Failed to open video decoder");
        avcodec_free_context(&vs->codecCtx);
        return -1;
    }
    
    ALOGI("✅ Video decoder opened: width=%d, height=%d, format=%d",
          vs->codecCtx->width, vs->codecCtx->height, vs->codecCtx->pix_fmt);
    
    return 0;
}

/// Освободить ресурсы видео декодера
///
/// @param vs Состояние видео
void video_decoder_destroy(VideoState *vs) {
    if (!vs) {
        return;
    }
    
    if (vs->codecCtx) {
        avcodec_free_context(&vs->codecCtx);
        vs->codecCtx = NULL;
    }
    
    ALOGI("✅ Video decoder destroyed");
}

/// Поток декодирования видео
///
/// Декодирует пакеты из PacketQueue и помещает decoded frames в FrameQueue
/// ❌ НЕ обновляет video_clock (это делает только render thread после eglSwapBuffers)
/// Обрабатывает EOF (Шаг 22)
static void *video_decode_thread(void *arg) {
    VideoState *vs = (VideoState *)arg;
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    
    if (!frame) {
        ALOGE("❌ video_decode_thread: Failed to allocate frame");
        return NULL;
    }
    
    PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
    
    ALOGI("🎞 Video decode loop started");
    
    while (!vs->abort) {
        // Извлекаем пакет из очереди (блокирующий)
        int ret = packet_queue_get(vs->packetQueue, &pkt, true);
        if (ret <= 0) {
            // EOF или abort (Шаг 22)
            // Помечаем video как завершённый
            if (ctx) {
                ctx->state.video_finished = 1;
                // Проверяем EOF (если и audio завершился)
                extern void handle_eof(PlayerContext *ctx);
                handle_eof(ctx);
            }
            break;
        }
        
        // 🔎 DIAGNOSTIC: Log packet received
        ALOGD("🎞 VideoDecoder: got packet pts=%lld", pkt.pts);
        
        // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.4: Фильтрация старых эпох
        // Если пакет из старой эпохи (serial не совпадает), дропаем его
        // Это предотвращает декодирование старых пакетов после seek
        if (ctx) {
            int current_serial = atomic_load(&ctx->seek_serial);
            // Пакеты не имеют serial, но мы проверяем seek.in_progress
            // Если seek в процессе, дропаем пакеты до тех пор, пока не найдём первый >= target
            if (ctx->seek.in_progress && ctx->seek.drop_video) {
                av_packet_unref(&pkt);
                continue;  // Дропаем пакет из старой эпохи
            }
        }
        
        // Отправляем пакет в декодер
        if (avcodec_send_packet(vs->codecCtx, &pkt) < 0) {
            av_packet_unref(&pkt);
            continue;
        }
        
        av_packet_unref(&pkt);
        
        // Получаем декодированные кадры
        while (!vs->abort) {
            ret = avcodec_receive_frame(vs->codecCtx, frame);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            
            if (ret < 0) {
                ALOGW("⚠️ video_decode_thread: Decode error %d", ret);
                break;
            }
            
            // 🔎 DIAGNOSTIC: Log frame decoded
            double pts_sec = NAN;
            if (vs->video_stream && vs->video_stream->time_base.num > 0 && vs->video_stream->time_base.den > 0) {
                if (frame->pts != AV_NOPTS_VALUE) {
                    pts_sec = frame->pts * av_q2d(vs->video_stream->time_base);
                } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    pts_sec = frame->best_effort_timestamp * av_q2d(vs->video_stream->time_base);
                }
            }
            ALOGI("🖼 VideoDecoder: frame decoded pts=%.3f size=%dx%d format=%d",
                  pts_sec,
                  frame->width,
                  frame->height,
                  frame->format);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.5: Передаём serial эпохи
            // Получаем текущий seek_serial из PlayerContext
            int current_serial = 0;
            if (ctx) {
                current_serial = atomic_load(&ctx->seek_serial);
            }
            
            // Вычисляем PTS в секундах
            double frame_pts = pts_sec;
            if (isnan(frame_pts) && vs->video_stream) {
                // Fallback на best_effort_timestamp или frame_index
                if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                    frame_pts = frame->best_effort_timestamp * av_q2d(vs->video_stream->time_base);
                } else {
                    // Используем frame_index как fallback
                    double fps = 25.0; // fallback FPS
                    if (vs->video_stream->avg_frame_rate.num > 0 && vs->video_stream->avg_frame_rate.den > 0) {
                        fps = av_q2d(vs->video_stream->avg_frame_rate);
                    }
                    frame_pts = vs->frame_index / fps;
                    vs->frame_index++;
                }
            }
            
            // Добавляем кадр в очередь (клонируется внутри frame_queue_push)
            // frame_queue_push принимает ownership кадра и клонирует его
            if (frame_queue_push(vs->frameQueue, frame, frame_pts, current_serial) < 0) {
                continue;
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: Сохраняем первый кадр для гарантированного рендера
            // Это критично для AVI и коротких файлов - первый кадр может быть потерян
            if (!vs->first_frame_ready) {
                if (vs->first_frame) {
                    av_frame_free(&vs->first_frame);
                }
                vs->first_frame = av_frame_clone(frame);
                if (vs->first_frame) {
                    vs->first_frame_ready = 1;
                    ALOGI("✅ video_decode_thread: First frame buffered (safety-net)");
                }
            }
        }
    }
    
    av_frame_free(&frame);
    ALOGI("🎞 Video decode loop finished");
    return NULL;
}

/// Запустить потоки декодирования и рендеринга видео
///
/// @param vs Состояние видео
/// @param as Состояние аудио (для A/V sync)
/// @return 0 при успехе, <0 при ошибке
int video_decode_thread_start(VideoState *vs, AudioState *as) {
    if (!vs) {
        ALOGE("❌ video_decode_thread_start: Invalid parameters");
        return -1;
    }
    
    if (vs->decodeThread_started) {
        ALOGW("⚠️ video_decode_thread_start: Decode thread already started");
        return 0;
    }
    
    if (!vs->packetQueue || !vs->frameQueue) {
        ALOGE("❌ video_decode_thread_start: packetQueue or frameQueue is NULL");
        return -1;
    }
    
    if (!vs->codecCtx) {
        ALOGE("❌ video_decode_thread_start: codecCtx is NULL");
        return -1;
    }
    
    vs->abort = 0;
    vs->decodeThread_joined = 0;
    
    int ret = pthread_create(&vs->decodeThread, NULL, video_decode_thread, vs);
    if (ret != 0) {
        ALOGE("❌ video_decode_thread_start: Failed to create decode thread: %d", ret);
        return -1;
    }
    
    vs->decodeThread_started = 1;
    ALOGI("✅ Video decode thread started");
    return 0;
}

/// Остановить потоки декодирования и рендеринга видео
///
/// @param vs Состояние видео
void video_threads_stop(VideoState *vs) {
    if (!vs) {
        return;
    }
    
    vs->abort = 1;
    
    // Прерываем очереди
    if (vs->packetQueue) {
        packet_queue_abort(vs->packetQueue);
    }
    if (vs->frameQueue) {
        frame_queue_abort(vs->frameQueue);
    }
    
    // 🔴 КРИТИЧНО: Ждём завершения decode thread ТОЛЬКО если он был запущен и ещё не join'нут
    ALOGI("🔄 video_threads_stop: decode valid=%d joined=%d tid=%p",
          vs->decodeThread_started, vs->decodeThread_joined, (void *)vs->decodeThread);
    
    if (vs->decodeThread_started && !vs->decodeThread_joined && vs->decodeThread != 0) {
        ALOGI("🔄 video_threads_stop: Joining decode thread (thread=%p)", (void *)vs->decodeThread);
        pthread_join(vs->decodeThread, NULL);
        vs->decodeThread_joined = 1;
        vs->decodeThread = 0;
        ALOGI("✅ video_threads_stop: Decode thread joined");
    } else {
        ALOGD("⚠️ video_threads_stop: Decode thread skip join (started=%d, joined=%d, thread=%p)",
              vs->decodeThread_started, vs->decodeThread_joined, (void *)vs->decodeThread);
    }
    
    ALOGI("✅ Video threads stopped");
}

/// Остановить потоки декодирования и рендеринга видео (алиас)
///
/// @param vs Состояние видео
void video_decode_thread_stop(VideoState *vs) {
    video_threads_stop(vs);
}
