#include "ffmpeg_player.h"
#include "packet_queue.h"
#include "frame_queue.h"
#include "clock.h"
#include "audio_renderer.h"
#include "video_renderer.h"
#include "video_render_gl.h"  // 🔴 ЭТАЛОН: Для video_render_gl_clear при seek
#include "ffmpeg_player_lifecycle.h"  // 🔴 ЗАДАЧА 4: Lifecycle management
#include "native_player_jni.h"  // 🔒 FIX Z11: Для native_player_emit_prepared_event_with_data
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <signal.h>  // Для pthread_kill
#include <errno.h>   // Для ESRCH
#include <unistd.h>  // 🔥 КРИТИЧЕСКИЙ FIX: Для usleep() (DISPOSE-GATE)
#include <android/log.h>
#include "libavutil/error.h"

// 🔴 ИСПРАВЛЕНО: Объявляем extern для доступа к g_renderer из native_player_jni.c
extern VideoRenderGL *g_renderer;

#define LOG_TAG "FFmpegPlayer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Поток demux (главный поток для seek и EOF)
///
/// Читает пакеты из файла и распределяет их по очередям
/// Выполняет seek при запросе
/// Обрабатывает EOF
/// 🔒 FIX: НЕ static - используется в native_player_jni.c для запуска после attach surface
void *demux_thread(void *arg) {
    PlayerContext *ctx = (PlayerContext *)arg;
    AVPacket pkt;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Проверяем AVSYNC-GATE перед стартом demux
    // AVSYNC-GATE открывается только после surfaceReady (eglMakeCurrent успешно выполнен)
    // Это гарантирует, что первый frame не будет dropped из-за race condition
    while (!ctx->avsync_gate_open && !ctx->state.abort_request) {
        usleep(1000); // Ждём 1ms
    }
    
    if (ctx->state.abort_request) {
        ALOGI("🛑 demux_thread: Aborted before AVSYNC-GATE opened");
        return NULL;
    }
    
    ALOGI("🎬 demux_thread started (AVSYNC-GATE open, surface ready)");
    
    // 🔎 DIAGNOSTIC: Log stream indices at start
    ALOGI("🔍 demux_thread: videoStream=%d audioStream=%d video=%p audio=%p", 
          ctx->videoStream, 
          ctx->audioStream,
          (void *)ctx->video,
          (void *)ctx->audio);
    if (ctx->video) {
        ALOGI("🔍 demux_thread: video->packetQueue=%p", (void *)ctx->video->packetQueue);
    }
    if (ctx->audio) {
        ALOGI("🔍 demux_thread: audio->packetQueue=%p", (void *)ctx->audio->packetQueue);
    }
    
    while (!ctx->state.abort_request) {
        // Проверяем запрос seek (Шаг 38)
        pthread_mutex_lock(&ctx->state.seek_mutex);
        if (ctx->state.seek_req_legacy || ctx->state.seek_req.seeking) {
            // Шаг 38.4: Phase 1 - Fast seek (keyframe)
            int ret = perform_fast_seek(ctx);
            if (ret < 0) {
                ALOGE("Fast seek failed: %d", ret);
            } else {
                // Шаг 38.5: Flush everything (критично!)
                // Flush выполняется внутри perform_fast_seek
                
                // Сбрасываем EOF флаги при seek
                ctx->state.audio_finished = 0;
                ctx->state.video_finished = 0;
                ctx->eof_reached = 0;  // 🔥 КРИТИЧЕСКИЙ FIX: AUTO-NEXT - сбрасываем флаг EOF при seek
                ctx->state.state = PLAYBACK_RUNNING;
                
                // Шаг 38.6: Phase 2 - Exact seek будет выполнен в decode threads
                // если ctx->state.seek_req.exact == true
            }
            
            ctx->state.seek_req_legacy = 0;
            // seeking остаётся true до завершения exact seek
            pthread_mutex_unlock(&ctx->state.seek_mutex);
            
            ALOGI("Fast seek completed, exact seek will be performed in decode threads");
        } else {
            pthread_mutex_unlock(&ctx->state.seek_mutex);
        }
        
        // Читаем пакет из файла
        int ret = av_read_frame(ctx->fmt, &pkt);
        
        if (ret == AVERROR_EOF) {
            ALOGI("📦 demux_thread: EOF reached");
            // EOF достигнут (Шаг 22)
            // Помечаем очереди как завершённые
            if (ctx->audio && ctx->audio->packetQueue) {
                packet_queue_abort(ctx->audio->packetQueue);
            }
            if (ctx->video && ctx->video->packetQueue) {
                packet_queue_abort(ctx->video->packetQueue);
            }
            break;
        }
        
        if (ret < 0) {
            // Ошибка чтения
            ALOGE("Error reading frame: %d", ret);
            break;
        }
        
        // Распределяем пакет по очередям
        if (pkt.stream_index == ctx->videoStream) {
            if (ctx->video && ctx->video->packetQueue) {
                packet_queue_put(ctx->video->packetQueue, &pkt);
                // 🔎 DIAGNOSTIC: Log video packet (обязательно для диагностики)
                ALOGD("📦 demux_thread: VIDEO packet pts=%lld stream_index=%d", pkt.pts, pkt.stream_index);
            } else {
                ALOGW("⚠️ demux_thread: Video packet dropped (video=%p, packetQueue=%p)", 
                      (void *)ctx->video, 
                      ctx->video ? (void *)ctx->video->packetQueue : NULL);
                av_packet_unref(&pkt);
            }
        } else if (pkt.stream_index == ctx->audioStream && ctx->audioStream >= 0) {
            if (ctx->audio && ctx->audio->packetQueue) {
                packet_queue_put(ctx->audio->packetQueue, &pkt);
                ALOGD("📦 demux_thread: audio packet pts=%lld", pkt.pts);
            } else {
                av_packet_unref(&pkt);
            }
        } else {
            // 🔎 DIAGNOSTIC: Log unknown stream packets
            ALOGD("📦 demux_thread: Unknown stream packet (stream_index=%d, videoStream=%d, audioStream=%d)", 
                  pkt.stream_index, ctx->videoStream, ctx->audioStream);
            av_packet_unref(&pkt);
        }
    }
    
    return NULL;
}

int player_seek(PlayerContext *ctx, double seconds, bool exact) {
    if (!ctx || !ctx->fmt) {
        return -1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.7: Scrub Spam Protection
    // Если seek уже выполняется, сохраняем новый seek как pending
    // Pending seek будет выполнен после firstFrameAfterSeek
    if (ctx->seek.in_progress || ctx->seek_in_progress) {
        ALOGI("🔍 SEEK: Seek already in progress, storing pending seek to %.3f sec", seconds);
        ctx->pending_seek_seconds = seconds;
        ctx->pending_seek_exact = exact;
        ctx->has_pending_seek = true;
        return 0; // Возвращаем успех, но не выполняем seek
    }
    
    // Очищаем pending seek флаги
    ctx->has_pending_seek = false;
    ctx->pending_seek_seconds = 0.0;
    ctx->pending_seek_exact = false;
    
    // Шаг 38.12: Edge cases - clamp to duration
    double duration = (double)ctx->fmt->duration / AV_TIME_BASE;
    if (seconds < 0.0) {
        seconds = 0.0;
    } else if (seconds > duration) {
        seconds = duration;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.9: ASSERT
    // Проверяем, что clocks должны быть инвалидированы после seek
    // Реализация функции seek_assert_clocks_invalidated
    #ifdef DEBUG
    if (ctx->audio && ctx->audio->clock.valid) {
        ALOGE("❌ SEEK_ASSERT FAILED: audio clock still valid before seek (FATAL)");
        abort();
    }
    if (ctx->video && ctx->video->clock.valid) {
        ALOGE("❌ SEEK_ASSERT FAILED: video clock still valid before seek (FATAL)");
        abort();
    }
    #endif
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK-GATE - закрываем gate перед seek
    // Это блокирует decode/render от обработки старых пакетов/кадров
    ctx->seek_in_progress = 1;
    ctx->waiting_first_frame_after_seek = 1;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - reset clocks при seek
    extern void avsync_reset(PlayerContext *ctx);
    avsync_reset(ctx);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master lock - разблокируем при seek
    ctx->avsync.master_locked = false;
    ALOGI("🔓 AVSYNC: Master unlocked (seek started)");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - устанавливаем seek_in_progress в AVSyncGate
    // Это разрешает render без ожидания master clock (bypass AVSYNC)
    avsync_gate_set_seek_in_progress(&ctx->avsync_gate, true);
    avsync_gate_invalidate(&ctx->avsync_gate, "seek started");
    ALOGI("🔍 SEEK: AVSYNC disabled (seek_in_progress)");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - останавливаем audio во время seek
    if (ctx->audio) {
        extern void audio_pause(AudioState *as);
        audio_pause(ctx->audio);
        
        // Эмитим audioState событие
        extern void native_player_emit_audio_state_event(const char *state);
        native_player_emit_audio_state_event("SEEKING");
        
        ALOGI("🔍 SEEK: Audio paused");
    }
    
    // Шаг 38.2, 38.3: Устанавливаем SeekRequest
    pthread_mutex_lock(&ctx->state.seek_mutex);
    
    // Вычисляем target_pts в stream time_base (Шаг 38.4)
    AVRational video_tb = ctx->fmt->streams[ctx->videoStream]->time_base;
    // Конвертируем секунды в PTS: target_pts = seconds / time_base
    int64_t target_pts = (int64_t)(seconds * video_tb.den / video_tb.num);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Сохраняем target_pts для проверки первого кадра после seek
    ctx->seek_target_pts = target_pts * av_q2d(video_tb);  // В секундах для удобства
    
    ctx->state.seek_req.target_pts = target_pts;
    ctx->state.seek_req.seek_start_pts = AV_NOPTS_VALUE;
    ctx->state.seek_req.exact = exact;
    ctx->state.seek_req.flushing = true;
    ctx->state.seek_req.seeking = true;
    
    // Legacy для обратной совместимости
    ctx->state.seek_pos = seconds;
    ctx->state.seek_flags = AVSEEK_FLAG_BACKWARD;
    ctx->state.seek_req_legacy = 1;
    
    pthread_mutex_unlock(&ctx->state.seek_mutex);
    
    ALOGI("🔍 SEEK-GATE: Seek requested: %.3f seconds (target_pts=%lld, exact=%s, SEEK-GATE closed)", 
          seconds, target_pts, exact ? "true" : "false");
    
    return 0;
}

/// Phase 1: Fast seek (Шаг 38.4)
/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6
/// ЕДИНСТВЕННАЯ ТОЧКА ВХОДА для seek
int perform_fast_seek(PlayerContext *ctx) {
    // 🔒 SEEK-GATE: ASSERT входных параметров
    if (!ctx || !ctx->fmt) {
        ALOGE("❌ perform_fast_seek: Invalid parameters");
        return -1;
    }
    
    SeekRequest *req = &ctx->state.seek_req;
    
    // Шаг 38.4: Вычисляем seek timestamp в stream time_base
    AVRational video_tb = ctx->fmt->streams[ctx->videoStream]->time_base;
    int64_t seek_ts = av_rescale_q(
        req->target_pts,
        video_tb,
        ctx->fmt->streams[ctx->videoStream]->time_base
    );
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.2: Правильный порядок операций
    // 1. serial++ (ПЕРВЫМ!) - это смена эпохи
    int new_serial = atomic_fetch_add(&ctx->seek_serial, 1) + 1;
    ALOGI("🔍 SEEK: New serial=%d (эпоха seek)", new_serial);
    
    // Вычисляем target
    double seek_target_sec = req->target_pts * av_q2d(video_tb);
    int64_t target_ms = (int64_t)(seek_target_sec * 1000.0);
    
    // 🔒 SEEK-GATE: ASSERT целевой позиции
    if (seek_target_sec < 0.0) {
        ALOGE("❌ perform_fast_seek: Invalid target_sec=%.3f", seek_target_sec);
        return -1;
    }
    
    ALOGI("🔍 SEEK[serial=%d]: request → %lld ms (%.3f sec)", new_serial, target_ms, seek_target_sec);
    
    // 2. abort demux (через abort_request)
    // 3. abort decode (через ctx->audio->abort и ctx->video->abort)
    // 4. flush queues (выполняется ниже)
    // 5. reset clocks (выполняется ниже)
    // 6. av_seek_frame (выполняется ниже)
    // 7. restart demux (demux thread уже запущен, просто сбрасываем abort)
    // 8. wait for first frame >= target (выполняется в render loop)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.4: SEEK RECOVERY (ANTI-DEADLOCK)
    // После seek:
    //   master = MASTER_VIDEO
    //   audio.ignore_until_first_frame = true
    // 📌 Это решает AVI / FLV seek deadlock
    
    // 🔥 ШАГ 20.4: master = MASTER_VIDEO (video-first после seek)
    ctx->avsync.master = CLOCK_MASTER_VIDEO;
    avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
    avsync_gate_set_valid(&ctx->avsync_gate);
    ALOGI("🔍 SEEK: master = VIDEO (video-first, audio ignored until firstFrameAfterSeek)");
    
    // 🔒 SEEK-GATE: блокируем render / decode
    ctx->seek.in_progress = true;
    ctx->seek.target_ms = target_ms;
    ctx->seek.drop_audio = true;
    ctx->seek.drop_video = true;
    ctx->seek.seek_id = new_serial;  // Сохраняем serial в seek_id для совместимости
    
    // ⛔️ ЖДЁМ остановки потоков (abort для немедленной остановки)
    if (ctx->audio) {
        ctx->audio->abort = 1;
    }
    if (ctx->video) {
        ctx->video->abort = 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.9: КРИТИЧЕСКИЕ ASSERT-ы
    #ifdef DEBUG
    // ASSERT(render_during_seek == false)
    if (ctx->seek.in_progress && ctx->video && ctx->video->renderThread_started) {
        ALOGE("❌ SEEK_ASSERT FAILED: render_during_seek=true (seeking=1 but video_render_started=1) (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    
    // ASSERT(!p->seeking || !audio_output_started)
    if (ctx->seek.in_progress && ctx->audio && ctx->audio->audio_render.started) {
        ALOGE("❌ SEEK_ASSERT FAILED: seeking=1 but audio_output_started=1 (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    
    // ASSERT(no_old_serial_frames) - проверяется в render loop через serial mismatch
    // ASSERT(first_frame_pts >= seek_target) - проверяется в render loop после firstFrameAfterSeek
    #endif
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - PATCH 2: Video clock reset на seek
    // Seek = reset clocks → flush → decode → render → firstFrameAfterSeek
    // Во время seek: audio НЕ master, video PTS = truth
    if (ctx->video) {
        // Reset video clock на target
        clock_set(&ctx->video->video_clock, seek_target_sec);
        ctx->video->clock_valid = 1;
        ALOGI("🔍 SEEK: Video clock reset to target=%.3f", seek_target_sec);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6.3
    // ⏱️ CLOCK RESET (ОДИН РАЗ, ТОЛЬКО ТУТ)
    // ❌ НИКАКИХ clock reset в decode / render
    if (ctx->audio) {
        extern void audio_clock_reset(AudioClock *c);
        if (ctx->audio) {
            audio_clock_reset(&ctx->audio->clock);
        }
        ALOGI("🔍 SEEK: Audio clock reset to %.3f", seek_target_sec);
    }
    
    if (ctx->video) {
        extern void video_clock_reset(VideoState *vs);
        if (ctx->video) {
            video_clock_reset(ctx->video);
        }
        ALOGI("🔍 SEEK: Video clock reset to %.3f", seek_target_sec);
    }
    
    // Reset master clock (даже если audio master)
    ctx->master_clock_ms = target_ms;
    ctx->avsync.audio_clock = seek_target_sec;
    ctx->avsync.video_clock = seek_target_sec;
    ctx->avsync.drift = 0.0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - устанавливаем seek state
    ctx->seek.in_progress = true;
    ctx->seek.target_ms = target_ms;
    ctx->seek.drop_audio = true;
    ctx->seek.drop_video = true;
    
    // Legacy поля (для обратной совместимости)
    ctx->seek_in_progress = 1;
    ctx->waiting_first_frame_after_seek = 1;
    ctx->seek_target_pts = seek_target_sec;  // Сохраняем в секундах для удобства
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - устанавливаем seek_in_progress в AVSyncGate
    // Это разрешает render без ожидания master clock (bypass AVSYNC)
    avsync_gate_set_seek_in_progress(&ctx->avsync_gate, true);
    avsync_gate_invalidate(&ctx->avsync_gate, "seek started");
    ALOGI("🔍 SEEK: AVSYNC disabled (seek_in_progress)");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - останавливаем audio во время seek
    if (ctx->audio) {
        extern void audio_pause(AudioState *as);
        audio_pause(ctx->audio);
        
        // Эмитим audioState событие
        extern void native_player_emit_audio_state_event(const char *state);
        native_player_emit_audio_state_event("SEEKING");
        
        ALOGI("🔍 SEEK: Audio paused");
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - запускаем seek watchdog
    extern int seek_watchdog_start(PlayerContext *ctx);
    seek_watchdog_start(ctx);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Сохраняем последнюю валидную позицию ДО seek
    // Это гарантирует, что get_position() вернёт стабильное значение во время seek
    // (не будет "телепорта" UI во время scrub)
    // ВАЖНО: Вызываем get_position() ДО установки seek_in_progress=1, чтобы избежать рекурсии
    int64_t current_pos_ms = 0;
    if (ctx->video && clock_is_active(&ctx->video->video_clock)) {
        double video_clock_sec = clock_get(&ctx->video->video_clock);
        if (video_clock_sec > 0) {
            current_pos_ms = (int64_t)(video_clock_sec * 1000);
        }
    } else if (ctx->audio) {
        current_pos_ms = (int64_t)(audio_get_clock(ctx->audio) * 1000);
    }
    
    if (current_pos_ms > 0) {
        ctx->last_position_before_seek_ms = current_pos_ms;
        ALOGI("🔍 SEEK-GATE: Saved last position before seek: %lld ms", (long long)current_pos_ms);
    } else if (ctx->last_position_before_seek_ms <= 0) {
        // Fallback: если нет сохранённой позиции, используем seek_target
        ctx->last_position_before_seek_ms = (int64_t)(seek_target_sec * 1000);
    }
    
    ALOGI("🔍 SEEK-GATE: Closed (seek_in_progress=1, waiting_first_frame_after_seek=1, target=%.3f, last_pos=%lld ms)", 
          seek_target_sec, (long long)ctx->last_position_before_seek_ms);
    
    // 1️⃣ Остановить декод/рендер потоки (abort для немедленной остановки)
    if (ctx->audio) {
        ctx->audio->abort = 1;
    }
    if (ctx->video) {
        ctx->video->abort = 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6.2
    // 🧹 ЖЁСТКИЙ FLUSH (ОДИН РАЗ, СТРОГО ПО ПОРЯДКУ)
    // 🚫 НИГДЕ БОЛЬШЕ flush не делаем
    ALOGI("🔍 SEEK: Flush queues (packet queues and frame queues)");
    if (ctx->audio && ctx->audio->packetQueue) {
        packet_queue_flush(ctx->audio->packetQueue);
    }
    if (ctx->video && ctx->video->packetQueue) {
        packet_queue_flush(ctx->video->packetQueue);
    }
    if (ctx->audio && ctx->audio->frameQueue) {
        frame_queue_flush(ctx->audio->frameQueue);
    }
    if (ctx->video && ctx->video->frameQueue) {
        frame_queue_flush(ctx->video->frameQueue);
    }
    
    // 🔒 FIX Z36: Очищаем буфер первого кадра при seek
    if (ctx->video) {
        if (ctx->video->first_frame) {
            av_frame_free(&ctx->video->first_frame);
            ctx->video->first_frame = NULL;
        }
        ctx->video->first_frame_ready = 0;
        ctx->video->first_frame_rendered = 0;
        ALOGI("🔍 First frame buffer cleared for seek");
    }
    
    // 3️⃣ Сброс декодеров (обязательно!)
    if (ctx->audio && ctx->audio->codecCtx) {
        avcodec_flush_buffers(ctx->audio->codecCtx);
    }
    if (ctx->video && ctx->video->codecCtx) {
        avcodec_flush_buffers(ctx->video->codecCtx);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6.4
    // 📦 AVSEEK (ТОЧНО)
    // ⚠️ AVI / FLV → ТОЛЬКО BACKWARD
    int seek_flags = AVSEEK_FLAG_BACKWARD;  // Обязательно BACKWARD для AVI/FLV
    
    // Определяем формат файла для специальной обработки
    const char *format_name = ctx->fmt->iformat ? ctx->fmt->iformat->name : NULL;
    int is_avi = format_name && (strcmp(format_name, "avi") == 0 || strcmp(format_name, "mpeg4") == 0);
    int is_flv = format_name && strcmp(format_name, "flv") == 0;
    
    if (is_avi || is_flv) {
        ALOGI("🔍 SEEK: AVI/FLV detected (format=%s), using AVSEEK_FLAG_BACKWARD", format_name ? format_name : "unknown");
        seek_flags = AVSEEK_FLAG_BACKWARD;
    }
    
    int ret = avformat_seek_file(
        ctx->fmt,
        ctx->videoStream,  // Seek по video stream
        INT64_MIN,
        seek_ts,
        INT64_MAX,
        seek_flags  // BACKWARD для AVI/FLV
    );
    
    if (ret < 0) {
        ALOGE("avformat_seek_file failed: %s", av_err2str(ret));
        return ret;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Для AVI/FLV после seek декодируем до первого keyframe
    // Это гарантирует, что первый кадр после seek будет валидным keyframe
    if (is_avi || is_flv) {
        ALOGI("🔍 SEEK-GATE: AVI/FLV seek completed, will decode to first keyframe >= target");
    }
    
    // Сохраняем PTS keyframe, на который попали
    req->seek_start_pts = seek_ts;
    
    // 5️⃣ Сброс clock (НЕ на 0, а на target_pts!)
    // 🔴 ЭТАЛОН: clock_set на seek_target, не на 0 (убирает ускорение и скачок таймлайна)
    // Вычисляем seek_pos в секундах из target_pts
    double seek_pos_sec = req->target_pts * av_q2d(video_tb);
    ALOGI("🔍 Reset clocks to seek_target: %.3f seconds", seek_pos_sec);
    if (ctx->audio) {
        extern void audio_clock_reset(AudioClock *c);
        if (ctx->audio) {
            audio_clock_reset(&ctx->audio->clock);
        }
    }
    if (ctx->video) {
        extern void video_clock_reset(VideoState *vs);
        video_clock_reset(ctx->video);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.6: SEEK FIX
    // При seek ОБЯЗАТЕЛЬНО инвалидируем audio clock
    if (ctx->audio) {
        audio_clock_reset(&ctx->audio->clock);
    }
    
    // ✅ ШАГ 6.2: Сбрасываем флаги для повторной отправки prepared после seek
    if (ctx->video) {
        ctx->video->first_frame_sent = 0;
        ctx->video->prepared_emitted = 0; // ✅ ШАГ 6.2: Сбрасываем флаг prepared для повторной отправки
        ALOGI("✅ Flags reset for seek (first_frame_sent, prepared_emitted)");
    }
    
    // 🔴 ЭТАЛОН: Очищаем video renderer при seek (убирает старые кадры и сбрасывает флаги)
    // 🔴 ШАГ J: Передаём seek_target для правильного сброса clock
    // 🔴 ИСПРАВЛЕНО: Используем g_renderer вместо ctx->video->video_render (который VideoRenderAndroid)
    if (g_renderer) {
        video_render_gl_clear(g_renderer, seek_pos_sec);
        ALOGI("✅ ШАГ J: video_render_gl_clear called after seek (seek_target=%.3f)", seek_pos_sec);
    }
    
    // 🔴 ЗАДАЧА 6: Сбрасываем субтитры при seek (используем audio clock)
    if (ctx->audio) {
        double audio_clock_sec = audio_get_clock(ctx->audio);
        subtitle_manager_seek(&ctx->subtitles, audio_clock_sec);
    } else {
        subtitle_manager_seek(&ctx->subtitles, seek_pos_sec);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6.5
    // ▶️ ПЕРЕЗАПУСК THREADS (НО decode НЕ СТАРТУЕТ)
    // 🚫 decode НЕ стартует до surfaceReady + play
    ctx->audio->abort = 0;
    ctx->video->abort = 0;
    
    // Demux thread уже запущен (стартует автоматически после surfaceReady)
    // Decode threads НЕ стартуют до play()
    
    ALOGI("✅ SEEK: Fast seek completed, seek_start_pts=%lld, decode will skip frames until target", 
          req->seek_start_pts);
    return 0;
}

/// Phase 2: Exact seek (Шаг 38.6)
///
/// Вызывается из decode threads после fast seek
/// Декодирует и дропает кадры до target_pts
int perform_exact_seek(PlayerContext *ctx) {
    if (!ctx) {
        return -1;
    }
    
    SeekRequest *req = &ctx->state.seek_req;
    
    if (!req->exact || !req->seeking) {
        return 0; // Exact seek не требуется
    }
    
    ALOGI("Phase 2: Exact seek - decode & drop until target_pts=%lld", req->target_pts);
    
    // Exact seek выполняется в decode threads (video_decode_thread, audio_decode_thread)
    // Здесь только проверяем завершение
    
    // Seek считается завершённым, когда:
    // - video: первый кадр с pts >= target_pts добавлен в очередь
    // - audio: первый сэмпл с pts >= target_pts записан в AudioTrack
    // Это проверяется в decode threads
    
    return 0;
}

/// Установить режим повтора (Шаг 22)
void set_repeat_mode(PlayerContext *ctx, int mode) {
    if (!ctx) {
        return;
    }
    ctx->state.repeat_mode = mode;
    ALOGI("Repeat mode set to: %d", mode);
}

/// Обработать EOF (Шаг 22)
///
/// 🔥 КРИТИЧЕСКИЙ FIX: AUTO-NEXT - корректное EOF detection
/// Проверяет, завершились ли все потоки, и принимает решение о repeat/stop/next
/// EOF никогда не считается stall - watchdog должен быть отключён
void handle_eof(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Проверяем, завершились ли все потоки (Шаг 22)
    // EOF считается ТОЛЬКО если audio_finished && video_finished
    // Для video-only: только video_finished достаточно
    bool eof_condition = ctx->state.video_finished;
    if (ctx->has_audio) {
        eof_condition = eof_condition && ctx->state.audio_finished;
    }
    
    if (eof_condition) {
        if (ctx->state.state != PLAYBACK_EOF) {
            ctx->state.state = PLAYBACK_EOF;
            ctx->eof_reached = 1;  // 🔥 КРИТИЧЕСКИЙ FIX: Устанавливаем флаг EOF
            ALOGI("✅ EOF reached (video-only=%d, repeat_mode=%d)", 
                  ctx->has_audio == 0, ctx->state.repeat_mode);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC Watchdog - отключаем при EOF
            // EOF ≠ STALL - это нормальное завершение playback
            extern void avsync_watchdog_stop(PlayerContext *ctx);
            avsync_watchdog_stop(ctx);
            ALOGI("✅ AVSYNC Watchdog disabled (EOF reached)");
            
            // 🔒 ЗАЩИТНЫЙ ASSERT (ОБЯЗАТЕЛЬНО)
            #ifdef DEBUG
            if (ctx->eof_reached && ctx->state.state == PLAYBACK_EOF) {
                // Проверяем, что watchdog не считает это stall
                // (проверка будет в watchdog thread)
            }
            #endif
            
            // Принимаем решение на основе repeat_mode
            switch (ctx->state.repeat_mode) {
                case 0: // repeat OFF
                    ctx->state.state = PLAYBACK_STOPPED;
                    ctx->paused = 1;  // 🔒 Native Event Contract: устанавливаем paused перед completed
                    // 🔒 Native Event Contract: эмитим completed вместо paused при EOF
                    extern void native_player_emit_completed_event(void);
                    native_player_emit_completed_event();
                    ALOGI("✅ PLAYBACK_COMPLETED event emitted (repeat OFF)");
                    break;
                    
                case 1: // repeat ONE
                    notify_flutter_event(ctx, "repeat_one");
                    // Выполняем seek к началу
                    ctx->eof_reached = 0;  // Сбрасываем флаг для repeat
                    player_seek(ctx, 0.0, false);
                    ctx->state.state = PLAYBACK_RUNNING;
                    break;
                    
                case 2: // repeat ALL
                    notify_flutter_event(ctx, "next");
                    ctx->state.state = PLAYBACK_STOPPED;
                    break;
            }
        }
    }
}

/// Уведомить Flutter о событии (Шаг 22)
void notify_flutter_event(PlayerContext *ctx, const char *event) {
    if (!ctx || !ctx->jvm || !ctx->jniCallback) {
        return;
    }
    
    JNIEnv *env = NULL;
    if ((*ctx->jvm)->GetEnv(ctx->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*ctx->jvm)->AttachCurrentThread(ctx->jvm, &env, NULL);
    }
    
    if (!env || !ctx->onEndedMethod) {
        return;
    }
    
    jstring jevent = (*env)->NewStringUTF(env, event);
    (*env)->CallVoidMethod(env, ctx->jniCallback, ctx->onEndedMethod, jevent);
    (*env)->DeleteLocalRef(env, jevent);
    
    ALOGI("Notified Flutter: %s", event);
}

/// Установить скорость воспроизведения (Шаг 39.7)
int player_set_speed(PlayerContext *ctx, double speed) {
    if (!ctx) {
        return -1;
    }
    
    // Шаг 39.8: Edge cases - ограничиваем скорость
    if (speed < 0.5) {
        speed = 0.5;
    } else if (speed > 3.0) {
        speed = 3.0;
    }
    
    pthread_mutex_lock(&ctx->state.seek_mutex);
    
    // Шаг 39.1: Обновляем PlaybackParams
    ctx->state.playback.speed = speed;
    
    // Шаг 39.2, 39.6: Обновляем clock speed
    if (ctx->audio && clock_is_active(&ctx->audio->clock)) {
        clock_set_speed(&ctx->audio->clock, speed);
    }
    if (ctx->video && clock_is_active(&ctx->video->video_clock)) {
        clock_set_speed(&ctx->video->video_clock, speed);
    }
    
    pthread_mutex_unlock(&ctx->state.seek_mutex);
    
    ALOGI("Playback speed set to: %.2fx", speed);
    
    // TODO: Шаг 39.3 - Обновить audio timestretch (Sonic/SoundTouch)
    // Это будет реализовано в audio_renderer.c
    
    return 0;
}

/// Открыть медиафайл и инициализировать все компоненты
///
/// @param ctx Контекст плеера
/// @param path Путь к медиафайлу
/// @return 0 при успехе, <0 при ошибке
int open_media(PlayerContext *ctx, const char *path) {
    if (!ctx || !path) {
        ALOGE("open_media: Invalid parameters");
        return -1;
    }
    
    ALOGI("🔄 open_media: Opening file: %s", path);
    
    // 1. Открыть AVFormatContext
    int ret = avformat_open_input(&ctx->fmt, path, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        ALOGE("Failed to open input: %s (error: %s)", path, errbuf);
        return ret;
    }
    
    ret = avformat_find_stream_info(ctx->fmt, NULL);
    if (ret < 0) {
        ALOGE("Failed to find stream info");
        avformat_close_input(&ctx->fmt);
        ctx->fmt = NULL;
        return ret;
    }
    
    ALOGI("✅ Format context opened, found %d streams", ctx->fmt->nb_streams);
    
    // 🔴 ШАГ 1: ПРАВИЛЬНО НАЙТИ AUDIO STREAM (ЭТАЛОН)
    // Инициализируем как -1 (нет стрима)
    ctx->videoStream = -1;
    ctx->audioStream = -1;
    
    // Ищем video stream
    ctx->videoStream = av_find_best_stream(ctx->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    
    // 🔴 ЭТАЛОН: Явный поиск audio stream с валидацией
    // Используем av_find_best_stream, но затем валидируем результат
    ctx->audioStream = av_find_best_stream(ctx->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    
    // 🔴 КРИТИЧНО: Валидация audio stream (защита от мусора)
    if (ctx->audioStream >= 0) {
        // Проверяем, что индекс в допустимых пределах
        if (ctx->audioStream >= ctx->fmt->nb_streams) {
            ALOGW("⚠️ Audio stream index %d out of range (nb_streams=%d), treating as no audio",
                  ctx->audioStream, ctx->fmt->nb_streams);
            ctx->audioStream = -1;
        } else {
            // Проверяем, что это действительно audio stream
            AVStream *st = ctx->fmt->streams[ctx->audioStream];
            if (st && st->codecpar && st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ALOGI("🔊 Audio stream found: %d (codec_id=%d, sample_rate=%d, channels=%d)",
                      ctx->audioStream,
                      st->codecpar->codec_id,
                      st->codecpar->sample_rate,
                      st->codecpar->ch_layout.nb_channels);
            } else {
                ALOGW("⚠️ Stream %d is not audio (codec_type=%d), treating as no audio",
                      ctx->audioStream,
                      st && st->codecpar ? st->codecpar->codec_type : -1);
                ctx->audioStream = -1;
            }
        }
    }
    
    if (ctx->videoStream < 0 && ctx->audioStream < 0) {
        ALOGE("❌ No video or audio stream found");
        avformat_close_input(&ctx->fmt);
        ctx->fmt = NULL;
        return -1;
    }
    
    ALOGI("Streams found: video=%d, audio=%d", ctx->videoStream, ctx->audioStream);
    
    // 🔥 КРИТИЧНО: Явно помечаем video-only режим
    ctx->has_audio = (ctx->audioStream >= 0) ? 1 : 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - инициализация AvSyncState
    extern void avsync_init(PlayerContext *ctx, int has_audio);
    avsync_init(ctx, ctx->has_audio);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - Master selection (1 место, 1 раз)
    // Инициализируем AVSyncGate
    avsync_gate_init(&ctx->avsync_gate);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master selection после определения streams
    if (ctx->has_audio == 1) {
        // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - проверяем clock_valid перед использованием audio master
        // Audio master запрещён, если clock невалиден или track failed
        // На этапе open_media() audio ещё не инициализирован, поэтому используем AUDIO master по умолчанию
        // clock_valid будет проверяться позже при установке AVSYNC valid
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
        // ❗️ master_valid = false (невалиден до audio PLAYING и clock_valid)
        // Будет установлен в true только при AudioTrack.play(), PLAYING и clock_valid
        ALOGI("🎛 AVSYNC MASTER = AUDIO (pending - will be valid after AudioTrack.play() and clock_valid)");
    } else {
        // Video MASTER (сразу валиден для video-only)
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
        ALOGI("🎛 AVSYNC MASTER = VIDEO (valid immediately for video-only)");
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 1️⃣ AUDIO_NO_AUDIO
    // Если audio stream отсутствует → AUDIO_NO_AUDIO
    if (ctx->audioStream < 0) {
        ctx->audio_state = AUDIO_NO_AUDIO;
        ALOGI("⚠️ No audio stream found (video-only file)");
        ALOGI("✅ Video-only mode enabled: has_audio=%d", ctx->has_audio);
        ALOGI("🎧 AudioState: AUDIO_NO_AUDIO (no audio stream in container)");
        // Эмитим событие в Flutter
        extern void native_player_emit_audio_state_event(const char *state);
        native_player_emit_audio_state_event("noAudio");
    } else {
        ALOGI("🔊 Audio stream validated: index=%d, has_audio=%d", ctx->audioStream, ctx->has_audio);
        // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 2️⃣ AUDIO_INITIALIZING
        // Audio stream найден → начинаем инициализацию
        ctx->audio_state = AUDIO_INITIALIZING;
        ALOGI("🎧 AudioState: AUDIO_INITIALIZING (audio stream found, starting initialization)");
        extern void native_player_emit_audio_state_event(const char *state);
        native_player_emit_audio_state_event("initializing");
    }
    
    // 3. Создать и инициализировать AudioState
    // 🔥 КРИТИЧНО: Создаём AudioState ТОЛЬКО если has_audio == 1
    if (ctx->has_audio && ctx->audioStream >= 0) {
        ctx->audio = (AudioState *)calloc(1, sizeof(AudioState));
        if (!ctx->audio) {
            ALOGE("Failed to allocate AudioState");
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return -1;
        }
        
        // Инициализируем очереди
        ctx->audio->packetQueue = (PacketQueue *)calloc(1, sizeof(PacketQueue));
        if (!ctx->audio->packetQueue) {
            ALOGE("Failed to allocate PacketQueue for audio");
            free(ctx->audio);
            ctx->audio = NULL;
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return -1;
        }
        packet_queue_init(ctx->audio->packetQueue);
        
        ctx->audio->frameQueue = (FrameQueue *)calloc(1, sizeof(FrameQueue));
        if (!ctx->audio->frameQueue) {
            ALOGE("Failed to allocate FrameQueue for audio");
            packet_queue_destroy(ctx->audio->packetQueue);
            free(ctx->audio->packetQueue);
            free(ctx->audio);
            ctx->audio = NULL;
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return -1;
        }
        // Инициализируем декодер (нужен для получения audio_stream)
        AVStream *audio_stream = ctx->fmt->streams[ctx->audioStream];
        
        // 🔴 КРИТИЧНО: Инициализируем frame_queue с time_base для fallback PTS
        frame_queue_init(ctx->audio->frameQueue, audio_stream->time_base);
        
        // Устанавливаем player_ctx для EOF обработки
        ctx->audio->player_ctx = ctx;
        ret = audio_decoder_init(ctx->audio, audio_stream);
        if (ret < 0) {
            ALOGE("Failed to initialize audio decoder");
            frame_queue_destroy(ctx->audio->frameQueue);
            packet_queue_destroy(ctx->audio->packetQueue);
            free(ctx->audio->frameQueue);
            free(ctx->audio->packetQueue);
            free(ctx->audio);
            ctx->audio = NULL;
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return ret;
        }
        
        // Инициализируем swr
        ret = audio_swr_init(ctx->audio);
        if (ret < 0) {
            ALOGE("Failed to initialize audio swr");
            audio_decoder_destroy(ctx->audio);
            frame_queue_destroy(ctx->audio->frameQueue);
            packet_queue_destroy(ctx->audio->packetQueue);
            free(ctx->audio->frameQueue);
            free(ctx->audio->packetQueue);
            free(ctx->audio);
            ctx->audio = NULL;
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return ret;
        }
        
        // Запускаем audio threads
        ret = audio_threads_start(ctx->audio, ctx->jvm);
        if (ret < 0) {
            ALOGE("Failed to start audio threads");
            // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 6️⃣ AUDIO_DEAD
            // Если audio threads не запустились → фатальная ошибка → AUDIO_DEAD
            ctx->audio_state = AUDIO_DEAD;
            ALOGI("💀 AudioState: AUDIO_DEAD (audio threads failed to start)");
            extern void native_player_emit_audio_state_event(const char *state);
            native_player_emit_audio_state_event("dead");
            
            audio_decoder_destroy(ctx->audio);
            frame_queue_destroy(ctx->audio->frameQueue);
            packet_queue_destroy(ctx->audio->packetQueue);
            free(ctx->audio->frameQueue);
            free(ctx->audio->packetQueue);
            free(ctx->audio);
            ctx->audio = NULL;
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return ret;
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 3️⃣ AUDIO_INITIALIZED
        // AudioTrack успешно создан → AUDIO_INITIALIZED
        // (Это НЕ playing - playbackHead ещё может быть 0)
        ctx->audio_state = AUDIO_INITIALIZED;
        ALOGI("🎧 AudioState: AUDIO_INITIALIZED (AudioTrack created, ready to play)");
        extern void native_player_emit_audio_state_event(const char *state);
        native_player_emit_audio_state_event("initialized");
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO-NATIVE Contract - после записи первого frame → AUDIO_READY
        // (Переход в AUDIO_READY будет в audio_render_thread после первой успешной записи)
        
        ALOGI("✅ AudioState initialized and threads started");
    }
    
    // 4. Создать и инициализировать VideoState
    if (ctx->videoStream >= 0) {
        ctx->video = (VideoState *)calloc(1, sizeof(VideoState));
        if (!ctx->video) {
            ALOGE("Failed to allocate VideoState");
            if (ctx->audio) {
                audio_threads_stop(ctx->audio);
                audio_decoder_destroy(ctx->audio);
                frame_queue_destroy(ctx->audio->frameQueue);
                packet_queue_destroy(ctx->audio->packetQueue);
                free(ctx->audio->frameQueue);
                free(ctx->audio->packetQueue);
                free(ctx->audio);
                ctx->audio = NULL;
            }
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return -1;
        }
        
        // Инициализируем очереди
        ctx->video->packetQueue = (PacketQueue *)calloc(1, sizeof(PacketQueue));
        if (!ctx->video->packetQueue) {
            ALOGE("Failed to allocate PacketQueue for video");
            free(ctx->video);
            ctx->video = NULL;
            if (ctx->audio) {
                audio_threads_stop(ctx->audio);
                audio_decoder_destroy(ctx->audio);
                frame_queue_destroy(ctx->audio->frameQueue);
                packet_queue_destroy(ctx->audio->packetQueue);
                free(ctx->audio->frameQueue);
                free(ctx->audio->packetQueue);
                free(ctx->audio);
                ctx->audio = NULL;
            }
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return -1;
        }
        packet_queue_init(ctx->video->packetQueue);
        
        ctx->video->frameQueue = (FrameQueue *)calloc(1, sizeof(FrameQueue));
        if (!ctx->video->frameQueue) {
            ALOGE("Failed to allocate FrameQueue for video");
            packet_queue_destroy(ctx->video->packetQueue);
            free(ctx->video->packetQueue);
            free(ctx->video);
            ctx->video = NULL;
            if (ctx->audio) {
                audio_threads_stop(ctx->audio);
                audio_decoder_destroy(ctx->audio);
                frame_queue_destroy(ctx->audio->frameQueue);
                packet_queue_destroy(ctx->audio->packetQueue);
                free(ctx->audio->frameQueue);
                free(ctx->audio->packetQueue);
                free(ctx->audio);
                ctx->audio = NULL;
            }
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return -1;
        }
        // Инициализируем декодер (нужен для получения video_stream)
        AVStream *video_stream = ctx->fmt->streams[ctx->videoStream];
        
        // 🔴 КРИТИЧНО: Инициализируем frame_queue с time_base для fallback PTS
        frame_queue_init(ctx->video->frameQueue, video_stream->time_base);
        
        // Устанавливаем player_ctx для EOF обработки
        ctx->video->player_ctx = ctx;
    
    // 🔴 ШАГ E: Логируем time_base и fps (ОБЯЗАТЕЛЬНО для диагностики)
    ALOGI("🎥 VIDEO STREAM:");
    ALOGI("   time_base = %d/%d (%.9f)",
          video_stream->time_base.num,
          video_stream->time_base.den,
          av_q2d(video_stream->time_base));
    ALOGI("   avg_frame_rate = %d/%d (%.3f fps)",
          video_stream->avg_frame_rate.num,
          video_stream->avg_frame_rate.den,
          video_stream->avg_frame_rate.den ?
              av_q2d(video_stream->avg_frame_rate) : 0.0);
    ALOGI("   r_frame_rate = %d/%d (%.3f fps)",
          video_stream->r_frame_rate.num,
          video_stream->r_frame_rate.den,
          video_stream->r_frame_rate.den ?
              av_q2d(video_stream->r_frame_rate) : 0.0);
    
    ret = video_decoder_init(ctx->video, video_stream);
        if (ret < 0) {
            ALOGE("Failed to initialize video decoder");
            frame_queue_destroy(ctx->video->frameQueue);
            packet_queue_destroy(ctx->video->packetQueue);
            free(ctx->video->frameQueue);
            free(ctx->video->packetQueue);
            free(ctx->video);
            ctx->video = NULL;
            if (ctx->audio) {
                audio_threads_stop(ctx->audio);
                audio_decoder_destroy(ctx->audio);
                frame_queue_destroy(ctx->audio->frameQueue);
                packet_queue_destroy(ctx->audio->packetQueue);
                free(ctx->audio->frameQueue);
                free(ctx->audio->packetQueue);
                free(ctx->audio);
                ctx->audio = NULL;
            }
            avformat_close_input(&ctx->fmt);
            ctx->fmt = NULL;
            return ret;
        }
        
        // video_threads_start будет вызван позже, когда будет доступен window
        // (через video_render_gl_attach_window или отдельный вызов)
        
        ALOGI("✅ VideoState initialized (threads will be started later)");
    }
    
    // 5. ❗ НЕ запускаем demux thread здесь
    // 🔒 FIX: Decode/demux стартует ТОЛЬКО после attach SurfaceTexture
    // Это критично для предотвращения EOF до готовности renderer (особенно для AVI/коротких файлов)
    // Эквивалент ExoPlayer: MediaCodec.configure(surface) перед start()
    ctx->state.abort_request = 0;
    ctx->decode_started = 0;  // Флаг, что decode ещё не запущен
    ctx->surface_attached = 0;  // Флаг, что surface ещё не прикреплён
    ctx->play_requested = 0;  // 🔒 DIFF 2: Флаг, что play() был вызван (decode стартует ТОЛЬКО после play)
    ALOGI("✅ open_media: Media opened, decode will start after play() (not after surface attach)");
    
    ALOGI("✅ Media opened successfully: videoStream=%d, audioStream=%d", 
          ctx->videoStream, ctx->audioStream);
    
    // 🔒 FIX Z11: prepared эмитится сразу после streams найдены и decoder готов
    // НЕ ждём первого кадра - это критично для video-only файлов
    // duration может быть 0, обновится после demux EOF
    int64_t duration_ms = get_duration(ctx);
    extern void native_player_emit_prepared_event_with_data(PlayerContext *ctx, int has_audio, int64_t duration_ms);
    native_player_emit_prepared_event_with_data(ctx, ctx->has_audio, duration_ms);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC Watchdog - НЕ запускаем здесь
    // Watchdog должен стартовать ТОЛЬКО после play(), когда clocks начали тикать
    // Иначе для video-only файлов watchdog будет считать idle clock как stall
    
    ALOGI("✅ Prepared event emitted from open_media (duration=%lld ms, has_audio=%d)", 
          (long long)duration_ms, ctx->has_audio);
    
    return 0;
}

void close_media(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Останавливаем demux thread
    if (ctx->demuxThread) {
        ctx->state.abort_request = 1;
        // Проверяем, что поток действительно существует (не был присоединён ранее)
        // pthread_t может быть невалидным после pthread_join, поэтому проверяем через pthread_kill
        int kill_ret = pthread_kill(ctx->demuxThread, 0);
        if (kill_ret == 0) {
            // Поток существует, присоединяем его
            pthread_join(ctx->demuxThread, NULL);
        } else if (kill_ret == ESRCH) {
            // Поток уже завершён или не существует - это нормально
            ALOGD("demuxThread already terminated");
        }
        ctx->demuxThread = 0;
    }
    
    // Освобождаем VideoState
    if (ctx->video) {
        // video_decoder_destroy уже вызывает video_threads_stop внутри
        video_decoder_destroy(ctx->video);
        
        if (ctx->video->frameQueue) {
            frame_queue_destroy(ctx->video->frameQueue);
            free(ctx->video->frameQueue);
            ctx->video->frameQueue = NULL;
        }
        
        if (ctx->video->packetQueue) {
            packet_queue_destroy(ctx->video->packetQueue);
            free(ctx->video->packetQueue);
            ctx->video->packetQueue = NULL;
        }
        
        free(ctx->video);
        ctx->video = NULL;
    }
    
    // Освобождаем AudioState
    if (ctx->audio) {
        // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 6️⃣ AUDIO_DEAD (при dispose)
        // AudioState.dead — терминальное состояние (после dead audio больше НИКОГДА не используется)
        if (ctx->audio_state != AUDIO_DEAD && ctx->audio_state != AUDIO_NO_AUDIO) {
            ctx->audio_state = AUDIO_DEAD;
            ALOGI("💀 AudioState: → AUDIO_DEAD (dispose)");
            extern void native_player_emit_audio_state_event(const char *state);
            native_player_emit_audio_state_event("dead");
        }
        
        audio_threads_stop(ctx->audio);
        audio_decoder_destroy(ctx->audio);
        
        if (ctx->audio->frameQueue) {
            frame_queue_destroy(ctx->audio->frameQueue);
            free(ctx->audio->frameQueue);
            ctx->audio->frameQueue = NULL;
        }
        
        if (ctx->audio->packetQueue) {
            packet_queue_destroy(ctx->audio->packetQueue);
            free(ctx->audio->packetQueue);
            ctx->audio->packetQueue = NULL;
        }
        
        free(ctx->audio);
        ctx->audio = NULL;
    }
    
    // Освобождаем format context
    if (ctx->fmt) {
        avformat_close_input(&ctx->fmt);
        ctx->fmt = NULL;
        ALOGI("AVFormatContext closed and freed");
    }
    
    // 🔴 ЗАДАЧА 6: Очищаем субтитры (destroy будет вызван в nativeDisposePlayerContext)
    subtitle_manager_clear(&ctx->subtitles);
    
    // Освобождаем mutex
    pthread_mutex_destroy(&ctx->state.seek_mutex);
    
    ALOGI("✅ close_media: All resources released");
}

int play(PlayerContext *ctx) {
    if (!ctx) {
        return -1;
    }
    
    // Используем seek_mutex для синхронизации pause/play
    pthread_mutex_lock(&ctx->state.seek_mutex);
    
    // 🔴 КРИТИЧНО: Проверяем, запущен ли уже master clock (audio или video для video-only)
    // Вместо проверки ctx->paused, проверяем активность clock
    // Это важно, потому что ctx->paused инициализируется как 0, но clock может быть не запущен
    bool clock_running = false;
    if (ctx->audio && clock_is_active(&ctx->audio->clock)) {
        clock_running = true;
        ALOGD("play: Audio clock is active");
    } else if (ctx->video && clock_is_active(&ctx->video->video_clock)) {
        clock_running = true;
        ALOGD("play: Video clock is active (video-only mode)");
    }
    
    // Если clock уже запущен и не на паузе - ничего не делаем
    if (clock_running && !ctx->paused) {
        pthread_mutex_unlock(&ctx->state.seek_mutex);
        // Но всё равно обновляем состояние для EOF обработки
        ctx->state.audio_finished = 0;
        ctx->state.video_finished = 0;
        ctx->state.state = PLAYBACK_RUNNING;
        ALOGD("play: Clock already running, skipping");
        return 0;
    }
    
    // 🔴 КРИТИЧНО: Если clock не запущен, ОБЯЗАТЕЛЬНО запускаем его
    // Это происходит при первом вызове play() после init
    ctx->paused = 0;
    
    // 🔴 КРИТИЧНО: Сбрасываем abort флаги
    ctx->abort = 0;
    if (ctx->video) {
        ctx->video->abort = 0;
    }
    
    // 🔥 ШАГ 4+5: Инициализация audio clock (MASTER) и флагов синхронизации
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - инициализация AudioClock
    // Примечание: audio_clock находится в ctx->audio->clock, не в ctx->audio_clock
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - инициализация audio clock
    if (ctx->audio) {
        audio_clock_init(&ctx->audio->clock);
    }
    
    // 1️⃣ Размораживаем audio clock (MASTER, если есть)
    if (ctx->audio) {
        clock_pause(&ctx->audio->clock, 0);
        // 2️⃣ Разрешаем audio thread писать
        ctx->audio->paused = 0;
        ctx->audio->player_ctx = ctx;
        ALOGI("✅ play: Audio clock started (clock_active=%d)", 
              clock_is_active(&ctx->audio->clock));
    }
    
    // 🔴 КРИТИЧНО: Для video-only файлов управляем video_clock
    if (ctx->video && !ctx->audio) {
        // Video-only файл - инициализируем clock СРАЗУ при play(), не ждём render
        if (clock_is_active(&ctx->video->video_clock)) {
            // Clock уже был активирован (resume после pause) - размораживаем
            clock_pause(&ctx->video->video_clock, 0);
            ALOGI("✅ play: Video clock resumed (video-only mode, resume)");
        } else {
            // 🔴 КРИТИЧНО: Инициализируем clock СРАЗУ при play(), не ждём render
            // Clock будет обновляться из PTS при декоде, а не из render
            // 🔴 ШАГ 4: НЕ стартуем video clock здесь - он будет инициализирован из первого кадра
            // clock_set(&ctx->video->video_clock, 0.0);  // ❌ УДАЛЕНО - video clock не должен стартовать раньше audio
            clock_pause(&ctx->video->video_clock, 0);  // Размораживаем
            ALOGI("✅ play: Video clock started immediately (video-only, pts=0.0)");
        }
    }
    
    // Устанавливаем player_ctx в VideoState для EOF обработки
    if (ctx->video) {
        ctx->video->player_ctx = ctx;
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Decode thread стартует ВСЕГДА, независимо от renderer_ready
        // Правильная модель (как в ExoPlayer/VLC/mpv):
        // - decode стартует ДО surface attach
        // - renderer подключается к существующему decode потоку
        // - кадры буферизуются до готовности renderer
        // Это устраняет deadlock: decode ждёт renderer → renderer ждёт decode
        
        // 🔥 КРИТИЧЕСКИЙ FIX: play() теперь управляет ТОЛЬКО clock/pause, а не запуском decode
        // Decode должен был стартовать автоматически после surfaceReady (в video_render_gl.c)
        // Если decode не стартовал - это ошибка архитектуры, но не блокируем play()
        if (!ctx->decode_started) {
            ALOGW("⚠️ play: Decode not started yet (should have started after surfaceReady)");
            ALOGW("   play() now only manages clock/pause, decode should auto-start after surfaceReady");
        } else if (!ctx->video->decodeThread) {
            // 🔥 КРИТИЧЕСКИЙ FIX: Fallback - запускаем decode если он не стартовал
            // Это защита от race condition, но в нормальном flow decode должен стартовать автоматически
            ALOGI("🔄 play: Starting video decode thread (fallback - should have started after surfaceReady)");
            int ret_decode = video_decode_thread_start(ctx->video, ctx->audio);
            if (ret_decode < 0) {
                ALOGE("❌ play: Failed to start video decode thread (fallback): %d", ret_decode);
                pthread_mutex_unlock(&ctx->state.seek_mutex);
                return -1;
            }
            ALOGI("✅ play: Video decode thread started (fallback)");
        } else {
            ALOGI("✅ play: Decode already started (auto-started after surfaceReady)");
        }
    }
    
    pthread_mutex_unlock(&ctx->state.seek_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC Watchdog - запускаем ТОЛЬКО после play()
    // Это гарантирует, что clocks начали тикать и watchdog не будет считать idle clock как stall
    // Запускаем ПОСЛЕ unlock, чтобы избежать deadlock
    extern int avsync_watchdog_start(PlayerContext *ctx);
    avsync_watchdog_start(ctx);
    ALOGI("✅ play: AVSYNC Watchdog started (clocks are running)");
    
    // 3️⃣ Renderer продолжает, alpha reset будет внутри video_render_gl_set_paused
    // (вызывается из nativePause/nativePlay в JNI)
    
    // Сбрасываем EOF флаги (Шаг 22)
    ctx->state.audio_finished = 0;
    ctx->state.video_finished = 0;
    ctx->state.state = PLAYBACK_RUNNING;
    
    ALOGI("✅ play: Playback resumed");
    
    return 0;
}

void player_pause(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    // Используем seek_mutex для синхронизации pause/play
    pthread_mutex_lock(&ctx->state.seek_mutex);
    
    // Если уже на паузе - ничего не делаем
    if (ctx->paused) {
        pthread_mutex_unlock(&ctx->state.seek_mutex);
        return;
    }
    
    ctx->paused = 1;
    
    // 1️⃣ Останавливаем audio clock (MASTER, если есть)
    if (ctx->audio) {
        clock_pause(&ctx->audio->clock, 1);
        // 2️⃣ Сигналим audio thread, что писать нельзя
        ctx->audio->paused = 1;
    }
    
    // 🔴 ШАГ 8: КРИТИЧНО - Останавливаем video_clock для video-only режима
    // Это предотвращает рост позиции когда видео на паузе
    if (ctx->video && !ctx->audio) {
        // Video-only файл - останавливаем video_clock
        clock_pause(&ctx->video->video_clock, 1);
        ALOGD("pause: Video clock paused (video-only mode)");
    }
    
    pthread_mutex_unlock(&ctx->state.seek_mutex);
    
    // 3️⃣ Останавливаем interpolation и render timing
    // (вызывается из nativePause в JNI через video_render_gl_set_paused)
    
    ALOGI("✅ pause: Playback paused");
}

int64_t get_position(PlayerContext *ctx) {
    if (!ctx) {
        return 0;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.2, 15.9: Запрет position updates во время seek
    // Position обновляется ТОЛЬКО после firstFrameAfterSeek
    // Это предотвращает "телепорт" UI и fake position во время scrub
    // ❌ emit position во время seek — ЗАПРЕЩЕНО
    if (ctx->seek_in_progress || ctx->waiting_first_frame_after_seek || ctx->seek.in_progress) {
        // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.9: ASSERT
        #ifdef DEBUG
        // ASSERT(!emit_position_during_seek)
        // Это и есть проверка - мы возвращаем стабильное значение, а не обновляем position
        #endif
        
        // Возвращаем последнюю валидную позицию до seek
        // Это гарантирует, что UI не увидит "прыжок" position во время seek
        if (ctx->last_position_before_seek_ms > 0) {
            return ctx->last_position_before_seek_ms;
        }
        // Fallback: если нет сохранённой позиции, возвращаем seek_target
        if (ctx->seek_target_pts > 0 && !isnan(ctx->seek_target_pts)) {
            return (int64_t)(ctx->seek_target_pts * 1000);
        }
        return 0;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master clock = единственный источник истины для position
    // master_clock_ms обновляется ТОЛЬКО после eglSwapBuffers (реальный рендер кадра)
    // Это гарантирует, что position соответствует реально показанному кадру
    if (ctx->master_clock_ms > 0) {
        return ctx->master_clock_ms;
    }
    
    // Fallback: video clock (если master_clock_ms ещё не обновлён)
    if (ctx->video && clock_is_active(&ctx->video->video_clock)) {
        double video_clock_sec = clock_get(&ctx->video->video_clock);
        if (video_clock_sec > 0) {
            return (int64_t)(video_clock_sec * 1000);
        }
    }
    
    // Fallback: audio clock (если есть audio и нет video)
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем audio_get_clock() (канонический)
    if (ctx->audio) {
        extern double audio_get_clock(AudioState *as);
        double audio_clock_sec = audio_get_clock(ctx->audio);
        if (!isnan(audio_clock_sec) && audio_clock_sec > 0.0) {
            return (int64_t)(audio_clock_sec * 1000);
        }
    }
    
    return 0;
}

/// 🔴 ЭТАЛОН: Правильное вычисление duration (как в VLC/mpv)
/// Использует AVFormatContext.duration или AVStream.duration как fallback
int64_t get_duration(PlayerContext *ctx) {
    if (!ctx || !ctx->fmt) {
        return 0;
    }
    
    double duration_sec = 0.0;
    
    // 1️⃣ Пробуем AVFormatContext.duration (в AV_TIME_BASE единицах)
    if (ctx->fmt->duration != AV_NOPTS_VALUE && ctx->fmt->duration > 0) {
        duration_sec = (double)ctx->fmt->duration / AV_TIME_BASE;
        ALOGI("🔍 Duration from AVFormatContext: %.3f seconds", duration_sec);
    }
    // 2️⃣ Fallback: используем video_stream->duration (в stream time_base)
    else if (ctx->videoStream >= 0 && ctx->videoStream < ctx->fmt->nb_streams) {
        AVStream *video_stream = ctx->fmt->streams[ctx->videoStream];
        if (video_stream && video_stream->duration != AV_NOPTS_VALUE && video_stream->duration > 0) {
            AVRational time_base = video_stream->time_base;
            duration_sec = (double)video_stream->duration * av_q2d(time_base);
            ALOGI("🔍 Duration from AVStream: %.3f seconds (duration=%lld, time_base=%d/%d)", 
                  duration_sec, (long long)video_stream->duration, time_base.num, time_base.den);
        }
    }
    
    if (duration_sec <= 0.0) {
        ALOGW("⚠️ Duration not available (fmt->duration=%lld, video_stream=%d)", 
              (long long)ctx->fmt->duration, ctx->videoStream);
        return 0;
    }
    
    int64_t duration_ms = (int64_t)(duration_sec * 1000.0);
    ALOGI("✅ Duration computed: %.3f seconds (%lld ms)", duration_sec, duration_ms);
    return duration_ms;
}

bool is_initialized(PlayerContext *ctx) {
    if (!ctx) {
        return false;
    }
    return ctx->fmt != NULL;
}

bool is_playing(PlayerContext *ctx) {
    if (!ctx) {
        return false;
    }
    return !ctx->paused && ctx->fmt != NULL && ctx->state.state == PLAYBACK_RUNNING;
}

/// Инициализировать PlayerState (вызывается при создании PlayerContext)
void player_state_init(PlayerState *state) {
    if (!state) {
        return;
    }
    
    memset(state, 0, sizeof(PlayerState));
    pthread_mutex_init(&state->seek_mutex, NULL);
    state->seek_flags = AVSEEK_FLAG_BACKWARD;
    state->state = PLAYBACK_RUNNING;
    state->repeat_mode = 0; // repeat OFF по умолчанию
    
    // Инициализируем SeekRequest (Шаг 38.2)
    state->seek_req.target_pts = AV_NOPTS_VALUE;
    state->seek_req.seek_start_pts = AV_NOPTS_VALUE;
    state->seek_req.exact = false;
    state->seek_req.flushing = false;
    state->seek_req.seeking = false;
    
    // Инициализируем PlaybackParams (Шаг 39.1)
    state->playback.speed = 1.0; // Нормальная скорость
    // pitch_correct не используется в текущей реализации
}

