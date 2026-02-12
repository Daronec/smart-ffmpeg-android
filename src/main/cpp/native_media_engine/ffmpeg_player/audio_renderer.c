#include "audio_renderer.h"
#include "audio_render_android.h"  // для audio_render_get_latency
#include "ffmpeg_player.h"
#include "avsync_gate.h"  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION
#include "libavutil/avutil.h"  // для AV_NOPTS_VALUE
#include "libavutil/frame.h"  // для frame->best_effort_timestamp
#include "libavutil/rational.h"  // для av_q2d
#include "libavutil/time.h"  // для av_gettime
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <stdbool.h>
#include <android/log.h>
#undef pause  // Убираем конфликт с системной функцией pause() из unistd.h

#define LOG_TAG "AudioRenderer"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - получить monotonic time в секундах
/// Используется для расчёта elapsed time в audio clock
static double get_monotonic_time_sec(void) {
    return (double)av_gettime_relative() / 1000000.0;  // микросекунды → секунды
}

// Макросы для FFmpeg
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
// FFMIN уже определён в libavutil/macros.h

/// Поток рендеринга аудио (MASTER CLOCK)
///
/// 🎯 ТОЛЬКО здесь обновляется audio_clock на основе samples_written
/// Извлекает PCM frames из FrameQueue и записывает в AudioTrack через audio_render_android
static void *audio_render_thread(void *arg) {
    AudioState *as = (AudioState *)arg;
    
    while (!as->abort) {
        // 🔴 ЗАДАЧА 1: Guard для pause - audio thread не пишет при паузе
        if (as->paused) {
            usleep(5000); // 5ms
            continue;
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Audio sync rules - audio не играет вперёд, догоняет видео
        // Проверяем, нужно ли ждать видео или дропнуть аудио
        bool audio_waiting_for_video = false;
        bool should_drop_audio = false;
        
        if (as->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)as->player_ctx;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем audio_get_clock()
            // Получаем текущий audio clock (канонический, PTS-based)
            extern double audio_get_clock(AudioState *as);
            double audio_clock_sec = audio_get_clock(ctx->audio);
            double audio_pts_ms = isnan(audio_clock_sec) ? 0.0 : audio_clock_sec * 1000.0;
            
            // Получаем master clock (video PTS) из PlayerContext
            double master_clock_ms = (double)ctx->master_clock_ms;
            
            // Вычисляем delta (разница между audio и video)
            double delta_ms = audio_pts_ms - master_clock_ms;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: Audio sync rule 1 - audio не играет вперёд
            // Если audio впереди video более чем на 40ms - ждём видео (fill_silence)
            if (delta_ms > 40.0) {
                audio_waiting_for_video = true;
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: Audio sync rule 2 - drop audio если сильно отстаёт
            // Если audio отстаёт от video более чем на 80ms - дропаем кадр
            if (delta_ms < -80.0) {
                should_drop_audio = true;
            }
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Если audio впереди видео - fill_silence вместо записи
        if (audio_waiting_for_video) {
            // Ждём видео - не записываем аудио, просто ждём
            usleep(5000); // 5ms
            continue;
        }
        
        // Извлекаем кадр из очереди (блокирующий)
        Frame af;
        if (frame_queue_pop(as->frameQueue, &af, true) <= 0) {
            continue;
        }
        
        AVFrame *frame = af.frame;
        double frame_pts = af.pts;  // PTS кадра (из decode thread)
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Если audio сильно отстаёт - дропаем кадр
        if (should_drop_audio) {
            ALOGW("🔊 Audio sync: dropping frame (audio behind video by > 80ms)");
            av_frame_free(&frame);
            continue;
        }
        
        // Получаем PCM данные из кадра
        int pcm_size = frame->nb_samples * as->channels * 2; // S16, stereo
        uint8_t *pcm = frame->data[0];
        
        // Записываем в AudioTrack (через audio_render_android)
        int written = audio_render_write(&as->audio_render, pcm, pcm_size);
        
        if (written > 0) {
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.2
            // 🔊 ГДЕ ОБНОВЛЯЕМ AUDIO CLOCK (ЕДИНСТВЕННОЕ МЕСТО)
            // после AudioTrack.write(...)
            // 🚫 НИГДЕ больше clock не трогаем
            
            // Получаем PTS фрейма (уже в секундах из decode thread)
            // frame_pts уже вычислен выше: double frame_pts = af.pts;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.2: ОБНОВЛЕНИЕ AUDIO CLOCK
            // Если PTS валиден - обновляем clock с учетом duration и latency
            if (!isnan(frame_pts) && frame_pts >= 0.0) {
                // Вычисляем duration фрейма
                double frame_duration = frame->nb_samples / (double)as->sample_rate;
                
                // Обновляем clock согласно каноническому определению:
                // audio_clock = last_audio_frame_pts + last_audio_frame_duration - audio_latency_compensation
                as->clock.last_pts = frame_pts;
                as->clock.last_duration = frame_duration;
                as->clock.last_update_us = av_gettime_relative();  // микросекунды
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.1: КАНОНИЧЕСКОЕ ОПРЕДЕЛЕНИЕ
                // audio_clock = last_audio_frame_pts + last_audio_frame_duration - audio_latency_compensation
                // Latency компенсируется, чтобы clock соответствовал тому, что пользователь СЛЫШИТ
                as->clock.clock = frame_pts + frame_duration - as->clock.latency;
                as->clock.valid = 1;
                
                // 🔍 ИНСТРУМЕНТАЦИЯ: логируем первые 10 обновлений
                static int log_count = 0;
                if (log_count < 10) {
                    ALOGD("🔊 AudioClock: clock=%.3f (pts=%.3f, duration=%.3f, latency=%.3f)", 
                          as->clock.clock, frame_pts, frame_duration, as->clock.latency);
                    log_count++;
                }
            }
            
            // Проверяем stall (Huawei case)
            audio_check_stall(as);
            
            // Обновляем PlayerContext->avsync.audio_clock из clock.clock
            if (as->player_ctx) {
                PlayerContext *ctx = (PlayerContext *)as->player_ctx;
                bool was_invalid = !ctx->avsync.audio_healthy;
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем clock.clock (не pts_sec)
                // Обновляем avsync.audio_clock из clock.clock (канонический audio clock)
                ctx->avsync.audio_clock = as->clock.clock;
                ctx->avsync.audio_healthy = as->clock.valid && !audio_clock_is_stalled(&as->clock);
                ctx->avsync.last_audio_clock = as->clock.clock;
                ctx->avsync.last_audio_clock_ts = (int64_t)(as->clock.last_update_us / 1000);  // microseconds → ms
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC IMPLEMENTATION - ШАГ 4
                // Если audio clock стал валидным → переключаем master на AUDIO
                if (was_invalid && ctx->avsync.audio_healthy) {
                    // Первый audio clock update → clock валиден → переключаем master на AUDIO
                    if (ctx->has_audio == 1) {
                        ctx->avsync.master = CLOCK_MASTER_AUDIO;
                        ALOGI("✅ AVSYNC: Master switch VIDEO → AUDIO (audio_clock became valid: %.3f)", 
                              ctx->avsync.audio_clock);
                        
                        // Обновляем AVSyncGate
                        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
                        avsync_gate_set_valid(&ctx->avsync_gate);
                    }
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - Audio clock advance (guarded)
                // Обновляем audio clock в AVSyncGate ТОЛЬКО если gate открыт
                if (avsync_gate_is_open(&ctx->avsync_gate)) {
                    int64_t clock_us = (int64_t)(as->clock.clock * 1000000.0);
                    avsync_gate_update_audio_clock(&ctx->avsync_gate, clock_us);
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - обновляем master switch логику
                extern void avsync_update(PlayerContext *ctx);
                avsync_update(ctx);
                
                // 🔍 ИНСТРУМЕНТАЦИЯ: логируем audio clock (первые 10 обновлений)
                static int audio_clock_log_count = 0;
                if (audio_clock_log_count < 10) {
                    ALOGD("🔊 AUDIO_CLOCK: %.3f (PTS-based, canonical)", as->clock.clock);
                    audio_clock_log_count++;
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.8: ASSERT (ОБЯЗАТЕЛЬНЫ)
                #ifdef DEBUG
                static double last_audio_clock = 0.0;
                // ASSERT(!isnan(audio_clock))
                if (as->clock.valid && isnan(as->clock.clock)) {
                    ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock is NAN (FATAL)");
                    abort(); // 🔥 FATAL в debug
                }
                // ASSERT(audio_clock >= 0)
                if (as->clock.valid && as->clock.clock < 0.0) {
                    ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock < 0 (%.3f) (FATAL)", as->clock.clock);
                    abort(); // 🔥 FATAL в debug
                }
                // ASSERT(audio_clock monotonic)
                if (as->clock.valid && !isnan(as->clock.clock) && as->clock.clock < last_audio_clock - 0.001) {
                    ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock regression (%.3f < %.3f) (FATAL)", 
                          as->clock.clock, last_audio_clock);
                    abort(); // 🔥 FATAL в debug
                }
                if (as->clock.valid && !isnan(as->clock.clock)) {
                    last_audio_clock = as->clock.clock;
                }
                
                if (!as->clock.valid) {
                    ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock is invalid");
                }
                #endif
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO-NATIVE Contract - точка перехода AUDIO_READY
            // После первой успешной записи → AUDIO_READY (buffer primed)
            if (as->player_ctx) {
                PlayerContext *ctx = (PlayerContext *)as->player_ctx;
                if (ctx->audio_state == AUDIO_INITIALIZED) {
                    ctx->audio_state = AUDIO_READY;
                    ALOGI("🎧 AudioState: AUDIO_INITIALIZED → AUDIO_READY (buffer primed, first frame written)");
                    extern void native_player_emit_audio_state_event(const char *state);
                    native_player_emit_audio_state_event("ready");
                }
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.5: ЧТО ЗАПРЕЩЕНО
            // ❌ Запрещено использовать getPlaybackHeadPosition как источник clock
            // ❌ Запрещено использовать systemClock
            // ❌ Запрещено использовать audio callback
            // ❌ Запрещено использовать таймеры
            // ❌ Запрещено использовать sleep
            // Clock обновляется ТОЛЬКО на основе PTS при write (см. код выше, где clock обновляется при written > 0)
            // Playback head используется ТОЛЬКО для диагностики AudioState, НЕ для clock
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - проверяем AudioTrack состояние
            // Используем getPlaybackHeadPosition ТОЛЬКО для диагностики, НЕ для clock
            if (as->player_ctx) {
                PlayerContext *ctx = (PlayerContext *)as->player_ctx;
                int64_t playback_head = audio_render_get_playback_head(&as->audio_render);
                static int64_t last_playback_head = 0;  // Для отслеживания роста playbackHead
                
                if (playback_head > last_playback_head) {
                    static int playback_head_updates = 0;
                    playback_head_updates++;
                    
                    // Переход в AUDIO_PLAYING только если playbackHead увеличился ≥ 2 раза
                    if (ctx->audio_state == AUDIO_INITIALIZED && playback_head_updates >= 2) {
                        ctx->audio_state = AUDIO_PLAYING;
                        ALOGI("🎧 AudioState: AUDIO_INITIALIZED → AUDIO_PLAYING (playbackHead advancing, updates=%d)", playback_head_updates);
                        extern void native_player_emit_audio_state_event(const char *state);
                        native_player_emit_audio_state_event("playing");
                    } else if (ctx->audio_state == AUDIO_STOPPED_BY_SYSTEM && playback_head > last_playback_head) {
                        // Переход stoppedBySystem → playing (AudioTrack возобновился)
                        ctx->audio_state = AUDIO_PLAYING;
                        playback_head_updates = 2;  // Сбрасываем счётчик
                        ALOGI("🎧 AudioState: AUDIO_STOPPED_BY_SYSTEM → AUDIO_PLAYING (AudioTrack resumed)");
                        extern void native_player_emit_audio_state_event(const char *state);
                        native_player_emit_audio_state_event("playing");
                    }
                    
                    last_playback_head = playback_head;
                } else if (playback_head == last_playback_head && ctx->audio_state == AUDIO_PLAYING) {
                    // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 5️⃣ AUDIO_STOPPED_BY_SYSTEM
                    // playbackHead замер → AudioTrack остановлен системой
                    static int frozen_count = 0;
                    static int64_t frozen_start_time = 0;
                    int64_t current_time = av_gettime() / 1000; // миллисекунды
                    
                    if (frozen_count == 0) {
                        frozen_start_time = current_time;
                    }
                    frozen_count++;
                    
                    // Если playbackHead замер > 1 секунды → AUDIO_STOPPED_BY_SYSTEM
                    if (current_time - frozen_start_time > 1000) {
                        ctx->audio_state = AUDIO_STOPPED_BY_SYSTEM;
                        ALOGW("⚠️ AudioState: AUDIO_PLAYING → AUDIO_STOPPED_BY_SYSTEM (playbackHead frozen for %ld ms)", 
                              (long)(current_time - frozen_start_time));
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - Audio exception = мгновенная смерть AVSYNC
                        avsync_gate_invalidate(&ctx->avsync_gate, "audio exception: playbackHead frozen");
                        
                        extern void native_player_emit_audio_state_event(const char *state);
                        native_player_emit_audio_state_event("stoppedBySystem");
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - инвалидируем clock при AudioTrack exception
                        // Инвалидируем audio clock при остановке системой
                        as->clock.valid = 0;
                        // Примечание: AudioClock не имеет поля stalled, используем valid=0 для индикации остановки
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - помечаем audio как unhealthy
                        ctx->avsync.audio_healthy = 0;
                        
                        // ⛔ STOP EVERYTHING - эмитим error событие
                        extern void native_player_emit_error_event(const char *message);
                        native_player_emit_error_event("AUDIO_MASTER_LOST");
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF - останавливаем playback при audio exception
                        extern void player_pause(PlayerContext *ctx);
                        player_pause(ctx);
                        
                        frozen_count = 0; // Сбрасываем счётчик
                    } else if (frozen_count == 1) {
                        ALOGW("⚠️ AudioState: playbackHead frozen (possible AUDIO_STOPPED_BY_SYSTEM, waiting for timeout)");
                    }
                }
                
                // Обновляем as->samples_written для обратной совместимости
                // Примечание: samples уже записаны через audio_render_write, используем frame->nb_samples
                if (written > 0 && frame) {
                    as->samples_written += frame->nb_samples;
                }
            }
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - Clock stall detector (обязательный)
        // Проверяем stall каждые 500ms (после обновления audio clock)
        if (as->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)as->player_ctx;
            static int64_t last_stall_check_us = 0;  // Статическая переменная для отслеживания последней проверки
            const int64_t stall_check_interval_us = 500000;  // 500ms в микросекундах
            int64_t now_us = av_gettime_relative(); // Используем av_gettime_relative для микросекунд
            if (last_stall_check_us == 0 || (now_us - last_stall_check_us >= stall_check_interval_us)) {
                if (avsync_gate_check_stall(&ctx->avsync_gate, 500000)) { // 500ms threshold
                    // Clock stall обнаружен → инвалидируем AVSYNC и эмитим error
                    avsync_gate_invalidate(&ctx->avsync_gate, "MASTER CLOCK STALLED");
                    extern void native_player_emit_error_event(const char *message);
                    native_player_emit_error_event("CLOCK_STALL");
                }
                last_stall_check_us = now_us;
            }
        }
        
        av_frame_free(&frame);
    }
    
    return NULL;
}

/// Поток декодирования аудио
///
/// Декодирует пакеты из PacketQueue и помещает decoded frames в FrameQueue
/// ❌ НЕ обновляет audio_clock (это делает только render thread)
/// Обрабатывает EOF (Шаг 22)
static void *audio_decode_thread(void *arg) {
    AudioState *as = (AudioState *)arg;
    AVPacket pkt;
    AVFrame *frame = av_frame_alloc();
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - drop until target reached
    PlayerContext *ctx = as->player_ctx;
    uint8_t **resampled_data = NULL;
    int resampled_linesize = 0;
    
    if (!frame) {
        return NULL;
    }
    
    while (!as->abort) {
        // Извлекаем пакет из очереди (блокирующий)
        int ret = packet_queue_get(as->packetQueue, &pkt, true);
        if (ret <= 0) {
            // EOF или abort (Шаг 22)
            // Помечаем audio как завершённый
            if (as->player_ctx) {
                PlayerContext *ctx = (PlayerContext *)as->player_ctx;
                ctx->state.audio_finished = 1;
                // Проверяем EOF (если и video завершился)
                handle_eof(ctx);
            }
            break;
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.4: Фильтрация старых эпох
        // Если пакет из старой эпохи (serial не совпадает), дропаем его
        // Это предотвращает декодирование старых пакетов после seek
        if (ctx) {
            int current_serial = atomic_load(&ctx->seek_serial);
            // Пакеты не имеют serial, но мы проверяем seek.in_progress
            // Если seek в процессе, дропаем пакеты до тех пор, пока не найдём первый >= target
            if (ctx->seek.in_progress && ctx->seek.drop_audio) {
                av_packet_unref(&pkt);
                continue;  // Дропаем пакет из старой эпохи
            }
        }
        
        // Отправляем пакет в декодер
        if (avcodec_send_packet(as->codecCtx, &pkt) < 0) {
            av_packet_unref(&pkt);
            continue;
        }
        
        av_packet_unref(&pkt);
        
        // Получаем декодированные кадры
        while (!as->abort) {
            int ret = avcodec_receive_frame(as->codecCtx, frame);
            
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            
            if (ret < 0) {
                goto end;
            }
            
            // 🔄 Resample
            // Вычисляем количество выходных сэмплов
            int out_samples = av_rescale_rnd(
                swr_get_delay(as->swr, as->codecCtx->sample_rate) + frame->nb_samples,
                as->codecCtx->sample_rate,
                as->codecCtx->sample_rate,
                AV_ROUND_UP
            );
            
            // Выделяем память для ресемплинга
            av_samples_alloc_array_and_samples(
                &resampled_data,
                &resampled_linesize,
                2, // stereo
                out_samples,
                AV_SAMPLE_FMT_S16,
                0
            );
            
            if (!resampled_data) {
                goto end;
            }
            
            // Выполняем ресемплинг
            int samples = swr_convert(
                as->swr,
                resampled_data,
                out_samples,
                (const uint8_t **)frame->data,
                frame->nb_samples
            );
            
            if (samples < 0) {
                av_freep(&resampled_data[0]);
                av_freep(&resampled_data);
                continue;
            }
            
            // 🎯 Применяем drift correction (Шаг 7)
            // Корректируем количество сэмплов для компенсации дрейфа
            int corrected_samples = audio_drift_correction_apply(as, samples);
            
            // Если коррекция изменила количество сэмплов, используем скорректированное значение
            if (corrected_samples != samples) {
                samples = corrected_samples;
            }
            
            // ❌ НЕ обновляем audio_clock здесь
            // audio_clock обновляется ТОЛЬКО в audio_render_thread на основе samples_written
            
            // 🧱 Push PCM frame в очередь (новый API)
            // Создаём выходной кадр
            AVFrame *out = av_frame_alloc();
            if (!out) {
                av_freep(&resampled_data[0]);
                av_freep(&resampled_data);
                continue;
            }
            
            out->format = AV_SAMPLE_FMT_S16;
            out->channel_layout = AV_CH_LAYOUT_STEREO;
            out->sample_rate = as->codecCtx->sample_rate;
            out->nb_samples = samples;
            
            if (av_frame_get_buffer(out, 0) < 0) {
                av_frame_free(&out);
                av_freep(&resampled_data[0]);
                av_freep(&resampled_data);
                continue;
            }
            
            // Копируем ресемпленные данные
            memcpy(out->data[0], resampled_data[0], samples * 2 * 2); // stereo, 16-bit
            
            // Вычисляем PTS
            int64_t frame_pts = frame->pts;
            double pts = frame_pts == AV_NOPTS_VALUE
                ? NAN
                : frame_pts * av_q2d(as->codecCtx->time_base);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - сохраняем last_written_pts
            // PTS is source of truth - сохраняем для обновления clock при write
            // ⚠️ НЕ обновляем clock здесь — только запоминаем PTS
            // Примечание: audio_clock находится в as->clock, не в ctx->audio_clock
            if (as->player_ctx && !isnan(pts) && pts >= 0.0) {
                // PTS сохраняется в as->clock.last_pts при обновлении clock
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6.7
            // 🔊 AUDIO: НИКОГДА НЕ ДАЁМ ЗВУК ДО VIDEO
            // 👉 Video — master для выхода из seek
            if (as->player_ctx) {
                PlayerContext *ctx = (PlayerContext *)as->player_ctx;
                
                if (ctx->seek.in_progress || ctx->seek.drop_audio) {
                    // ⛔️ audio silence until video first frame after seek
                    ALOGD("🔍 SEEK: dropping audio frame (audio silence until video first frame)");
                    av_frame_free(&out);
                    av_freep(&resampled_data[0]);
                    av_freep(&resampled_data);
                    continue; // DROP
                }
                
                // После завершения seek (video нашёл первый кадр) - разрешаем audio
                if (!ctx->seek.in_progress && ctx->seek.drop_audio) {
                    ctx->seek.drop_audio = false;
                    ALOGI("✅ SEEK: Audio drop disabled (video first frame found)");
                }
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.5: Передаём serial эпохи
            // Получаем текущий seek_serial из PlayerContext
            int current_serial = 0;
            if (ctx) {
                current_serial = atomic_load(&ctx->seek_serial);
            }
            
            // Добавляем кадр в очередь (клонируется внутри) с serial эпохи
            if (frame_queue_push(as->frameQueue, out, pts, current_serial) < 0) {
                av_frame_free(&out);
                av_freep(&resampled_data[0]);
                av_freep(&resampled_data);
                continue;
            }
            
            // Освобождаем локальный кадр (он клонирован в очереди)
            av_frame_free(&out);
            av_freep(&resampled_data[0]);
            av_freep(&resampled_data);
        }
    }
    
end:
    av_frame_free(&frame);
    if (resampled_data) {
        av_freep(&resampled_data[0]);
        av_freep(&resampled_data);
    }
    return NULL;
}

int audio_decoder_init(AudioState *as, AVStream *stream) {
    if (!as || !stream) {
        ALOGE("❌ audio_decoder_init: Invalid parameters");
        return -1;
    }
    
    // 🔴 ШАГ 2: ИНИЦИАЛИЗАЦИЯ AUDIO DECODER (ЭТАЛОН)
    // Находим декодер
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        ALOGE("❌ audio_decoder_init: Codec not found (codec_id=%d)", stream->codecpar->codec_id);
        return -1;
    }
    
    ALOGI("🔊 Audio decoder found: %s", codec->name);
    
    // Выделяем codec context
    as->codecCtx = avcodec_alloc_context3(codec);
    if (!as->codecCtx) {
        ALOGE("❌ audio_decoder_init: Failed to allocate codec context");
        return -1;
    }
    
    // Копируем параметры из стрима
    if (avcodec_parameters_to_context(as->codecCtx, stream->codecpar) < 0) {
        ALOGE("❌ audio_decoder_init: Failed to copy codec parameters");
        avcodec_free_context(&as->codecCtx);
        return -1;
    }
    
    // Открываем декодер
    if (avcodec_open2(as->codecCtx, codec, NULL) < 0) {
        ALOGE("❌ audio_decoder_init: Failed to open audio decoder");
        avcodec_free_context(&as->codecCtx);
        return -1;
    }
    
    ALOGI("✅ Audio decoder opened: sample_rate=%d, channels=%d, format=%d",
          as->codecCtx->sample_rate,
          #if LIBAVCODEC_VERSION_MAJOR >= 60
          as->codecCtx->ch_layout.nb_channels,
          #else
          as->codecCtx->channels,
          #endif
          as->codecCtx->sample_fmt);
    
    // Сохраняем параметры
    as->sample_rate = as->codecCtx->sample_rate;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 7
    // Инициализация AudioClock структуры (PTS-based)
    memset(&as->clock, 0, sizeof(AudioClock));
    as->clock.clock = NAN;
    as->clock.last_pts = NAN;
    as->clock.last_duration = 0.0;
    as->clock.latency = 0.0;
    as->clock.last_update_us = 0;
    as->clock.valid = 0;
    
    // Legacy поля (deprecated)
    as->audio_clock_pts = NAN;
    as->last_audio_clock_pts = NAN;
    
    // Для FFmpeg 6.0+ используем ch_layout, для старых версий - channels
    #if LIBAVCODEC_VERSION_MAJOR >= 60
        as->channels = as->codecCtx->ch_layout.nb_channels;
    #else
        as->channels = as->codecCtx->channels;
    #endif
    as->sample_fmt = AV_SAMPLE_FMT_S16; // AudioTrack любит S16
    
    // Инициализируем audio clock и samples_written (Шаг 11)
    as->audio_clock = 0.0;
    as->audio_pts_start = 0.0;
    as->samples_written = 0;
    as->playback_head_samples = 0;
    as->abort = 0;
    as->paused = 0;
    as->out_buf = NULL;
    as->out_buf_size = 0;
    as->jvm = NULL;
    
    // Инициализируем clock (Шаг 20)
    clock_init(&as->clock);
    
    // Инициализируем drift correction
    audio_drift_correction_init(as);
    
    // audio_render будет инициализирован в audio_threads_start
    
    ALOGI("✅ audio_decoder_init: Audio decoder initialized successfully");
    return 0;
}

int audio_swr_init(AudioState *as) {
    if (!as || !as->codecCtx) {
        ALOGE("❌ audio_swr_init: Invalid parameters");
        return -1;
    }
    
    AVCodecContext *c = as->codecCtx;
    
    // 🔴 ШАГ 5: SWR → PCM (ЕСЛИ НЕ PCM) (ЭТАЛОН)
    // Всегда используем swr, даже если формат "совпадает"
    // (иначе сломается на другом устройстве)
    // Используем swr_alloc_set_opts для совместимости
    // Output: stereo, S16
    uint64_t out_ch_layout = AV_CH_LAYOUT_STEREO;
    
    // Input: используем channel_layout из codec context
    // Для FFmpeg 6.0+ используем ch_layout, для старых версий - channel_layout
    uint64_t in_ch_layout;
    #if LIBAVCODEC_VERSION_MAJOR >= 60
        in_ch_layout = c->ch_layout.u.mask;
    #else
        in_ch_layout = c->channel_layout;
    #endif
    
    ALOGI("🔊 SWR init: in_ch_layout=%llu, in_fmt=%d, in_rate=%d → out_ch_layout=%llu, out_fmt=%d, out_rate=%d",
          (unsigned long long)in_ch_layout, c->sample_fmt, c->sample_rate,
          (unsigned long long)out_ch_layout, as->sample_fmt, as->sample_rate);
    
    as->swr = swr_alloc_set_opts(
        NULL,
        out_ch_layout,            // output: stereo
        as->sample_fmt,           // output: S16
        as->sample_rate,          // output: same sample rate
        in_ch_layout,             // input: original channel layout
        c->sample_fmt,            // input: original sample format
        c->sample_rate,           // input: original sample rate
        0,                        // log offset
        NULL                      // log context
    );
    
    if (!as->swr) {
        ALOGE("❌ audio_swr_init: Failed to allocate SWR context");
        return -1;
    }
    
    if (swr_init(as->swr) < 0) {
        ALOGE("❌ audio_swr_init: Failed to initialize SWR context");
        swr_free(&as->swr);
        return -1;
    }
    
    ALOGI("✅ SWR initialized successfully");
    
    // Выделяем буфер для ресемплинга
    as->out_buf_size = av_samples_get_buffer_size(
        NULL,
        as->channels,
        c->frame_size > 0 ? c->frame_size : 4096,
        as->sample_fmt,
        0
    );
    
    if (as->out_buf_size < 0) {
        swr_free(&as->swr);
        return -1;
    }
    
    as->out_buf = (uint8_t *)av_malloc(as->out_buf_size);
    if (!as->out_buf) {
        swr_free(&as->swr);
        return -1;
    }
    
    return 0;
}

int audio_threads_start(AudioState *as, JavaVM *jvm) {
    if (!as || !jvm) {
        ALOGE("❌ audio_threads_start: Invalid parameters");
        return -1;
    }
    
    as->jvm = jvm;
    as->abort = 0;
    
    // 🔴 ШАГ 6: ANDROID AudioTrack (ЭТАЛОН)
    // Инициализируем AudioTrack
    ALOGI("🔊 Initializing AudioTrack: sample_rate=%d, channels=%d", as->sample_rate, as->channels);
    if (!audio_render_init(&as->audio_render, jvm, as->sample_rate, as->channels)) {
        ALOGE("❌ audio_threads_start: Failed to initialize AudioTrack");
        return -1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Инициализация AudioClock
    audio_clock_init(&as->clock);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.3: AUDIO LATENCY
    // Вычисляем latency один раз при инициализации
    // latency = audio_buffer_size / (sample_rate * channels * bytes_per_sample)
    // Или используем AudioTrack.getLatency() если доступен
    int latency_ms = audio_render_get_latency(&as->audio_render);
    if (latency_ms > 0) {
        as->clock.latency = latency_ms / 1000.0;  // ms → seconds
        ALOGI("🔊 AudioClock: Latency initialized: %.3f sec (%d ms)", as->clock.latency, latency_ms);
    } else {
        // Fallback: вычисляем latency из buffer size
        // AudioTrack buffer size обычно ~100-200ms
        // Используем консервативную оценку 100ms
        as->clock.latency = 0.1;  // 100ms fallback
        ALOGW("⚠️ AudioClock: Latency not available, using fallback: %.3f sec", as->clock.latency);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO-NATIVE Contract - передаём PlayerContext в AudioRenderAndroid
    // Это нужно для обновления audio_state при подтверждении PLAYSTATE_PLAYING
    if (as->player_ctx) {
        as->audio_render.player_ctx = as->player_ctx;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: MUTE-AUDIO ASSERT - проверяем громкость перед AudioTrack.play()
    // Если громкость = 0%, AudioTrack может быть остановлен системой (onAudioException)
    // Это не ошибка, но важно для диагностики silent playback
    // Проверка выполняется через JNI (если доступно) или только логируется
    ALOGI("🔊 MUTE-AUDIO ASSERT: Checking system volume before AudioTrack.play()");
    ALOGI("   If volume = 0%%, AudioTrack may be stopped by Android AudioSystem");
    ALOGI("   This is expected behavior on some OEM devices (Huawei/HiSilicon)");
    
    // Запускаем AudioTrack
    audio_render_start(&as->audio_render);
    ALOGI("✅ AudioTrack started");
    
    // 🔴 ШАГ 4: AUDIO DECODE THREAD (ЭТАЛОН)
    // Запускаем decode thread
    ALOGI("🔊 Starting audio decode thread...");
    if (pthread_create(&as->decodeThread, NULL, audio_decode_thread, as) != 0) {
        ALOGE("❌ audio_threads_start: Failed to create audio decode thread");
        audio_render_release(&as->audio_render);
        return -1;
    }
    
    // 🔴 КРИТИЧНО: Устанавливаем флаги состояния thread
    as->decodeThread_started = 1;
    as->decodeThread_joined = 0;
    ALOGI("✅ Audio decode thread started");
    
    // Запускаем render thread
    ALOGI("🔊 Starting audio render thread...");
    if (pthread_create(&as->renderThread, NULL, audio_render_thread, as) != 0) {
        ALOGE("❌ audio_threads_start: Failed to create audio render thread");
        as->abort = 1;
        if (as->packetQueue) {
            packet_queue_abort(as->packetQueue);
        }
        // 🔴 КРИТИЧНО: Join decode thread только если он был запущен
        if (as->decodeThread_started && !as->decodeThread_joined && as->decodeThread) {
            pthread_join(as->decodeThread, NULL);
            as->decodeThread_joined = 1;
            as->decodeThread = 0;
        }
        audio_render_release(&as->audio_render);
        return -1;
    }
    
    // 🔴 КРИТИЧНО: Устанавливаем флаги состояния render thread
    as->renderThread_started = 1;
    as->renderThread_joined = 0;
    
    ALOGI("✅ Audio threads started (decode + render)");
    return 0;
}

void audio_threads_stop(AudioState *as) {
    if (!as) {
        return;
    }
    
    // 🔴 КРИТИЧНО: Защита от повторного вызова
    // Если threads уже были остановлены, не делаем ничего
    if (as->threads_stopped) {
        ALOGD("⚠️ audio_threads_stop: Already called for this AudioState, skipping");
        return;
    }
    
    as->abort = 1;
    
    // Прерываем очереди
    if (as->packetQueue) {
        packet_queue_abort(as->packetQueue);
    }
    if (as->frameQueue) {
        frame_queue_abort(as->frameQueue);
    }
    
    // Останавливаем AudioTrack
    audio_render_stop(&as->audio_render);
    
    // 🔴 КРИТИЧНО: Ждём завершения потоков ТОЛЬКО если они были запущены и ещё не join'нуты
    // ЗОЛОТОЕ ПРАВИЛО: pthread_join можно вызывать ТОЛЬКО если:
    // - thread был создан (decodeThread_started == 1)
    // - thread ещё НЕ был joined (decodeThread_joined == 0)
    // - thread != 0 (валидный pthread_t)
    // - join вызывается ОДИН раз
    ALOGI("🔄 audio_threads_stop: decode valid=%d joined=%d tid=%p",
          as->decodeThread_started, as->decodeThread_joined, (void *)as->decodeThread);
    
    if (as->decodeThread_started && !as->decodeThread_joined && as->decodeThread != 0) {
        ALOGI("🔄 audio_threads_stop: Joining decode thread (thread=%p)", (void *)as->decodeThread);
        pthread_join(as->decodeThread, NULL);
        as->decodeThread_joined = 1;
        as->decodeThread = 0;
        ALOGI("✅ audio_threads_stop: Decode thread joined");
    } else {
        ALOGD("⚠️ audio_threads_stop: Decode thread skip join (started=%d, joined=%d, thread=%p)",
              as->decodeThread_started, as->decodeThread_joined, (void *)as->decodeThread);
    }
    
    ALOGI("🔄 audio_threads_stop: render valid=%d joined=%d tid=%p",
          as->renderThread_started, as->renderThread_joined, (void *)as->renderThread);
    
    if (as->renderThread_started && !as->renderThread_joined && as->renderThread != 0) {
        ALOGI("🔄 audio_threads_stop: Joining render thread (thread=%p)", (void *)as->renderThread);
        pthread_join(as->renderThread, NULL);
        as->renderThread_joined = 1;
        as->renderThread = 0;
        ALOGI("✅ audio_threads_stop: Render thread joined");
    } else {
        ALOGD("⚠️ audio_threads_stop: Render thread skip join (started=%d, joined=%d, thread=%p)",
              as->renderThread_started, as->renderThread_joined, (void *)as->renderThread);
    }
    
    // Освобождаем AudioTrack
    audio_render_release(&as->audio_render);
    
    // 🔴 КРИТИЧНО: Устанавливаем флаг, что threads были остановлены
    as->threads_stopped = 1;
    ALOGI("✅ Audio threads stopped");
}

void audio_decoder_destroy(AudioState *as) {
    if (!as) {
        return;
    }
    
    // Останавливаем потоки (освобождает AudioTrack внутри через audio_render_release)
    audio_threads_stop(as);
    
    // Освобождаем буфер
    if (as->out_buf) {
        av_freep(&as->out_buf);
    }
    
    // Освобождаем swr
    if (as->swr) {
        swr_free(&as->swr);
    }
    
    // Освобождаем codec context
    if (as->codecCtx) {
        avcodec_free_context(&as->codecCtx);
    }
    
    memset(as, 0, sizeof(AudioState));
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.4: EXTRAPOLATION
/// ⛔ ЗАПРЕЩАЕМ playbackHead как источник
/// 
/// @param as Состояние аудио
/// @return Текущий audio clock в секундах (PTS-based с extrapolation)
///
/// Если нет новых фреймов, экстраполируем на основе elapsed time
/// ТОЛЬКО если playing (не paused, не stalled)
/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.1: audio_clock_now()
/// Audio clock = AudioTrack.getPlaybackHeadPosition()
/// ❌ PTS больше НЕ используется как master clock
static double audio_clock_now(AudioState *as) {
    if (!as || !as->audio_render.audio_track || !as->audio_render.started) {
        return NAN;
    }
    
    // 🔥 ШАГ 20.1: Используем AudioTrack.getPlaybackHeadPosition()
    extern int64_t audio_render_get_playback_head(AudioRenderAndroid *ar);
    uint32_t frames = (uint32_t)audio_render_get_playback_head(&as->audio_render);
    
    if (frames == 0 && as->audio_render.started) {
        // AudioTrack может вернуть 0 если только что запущен или остановлен
        // Проверяем play state
        extern int audio_render_get_play_state(AudioRenderAndroid *ar);
        int play_state = audio_render_get_play_state(&as->audio_render);
        if (play_state != 3) {  // PLAYSTATE_PLAYING = 3
            return NAN;  // AudioTrack не играет
        }
    }
    
    // Конвертируем frames в секунды
    double clock_sec = frames / (double)as->sample_rate;
    
    return clock_sec;
}

double audio_get_clock(AudioState *as) {
    if (!as) {
        return NAN;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.1: Audio clock = AudioTrack.getPlaybackHeadPosition()
    // ✅ Используем playbackHead как источник правды
    // ❌ PTS больше НЕ используется как master clock
    double clock_sec = audio_clock_now(as);
    
    // Обновляем last_clock_update для stall detection
    if (!isnan(clock_sec)) {
        as->clock.clock = clock_sec;
        as->clock.valid = 1;
        as->clock.last_update_us = av_gettime_relative();
    } else {
        // AudioTrack не играет или остановлен
        as->clock.valid = 0;
    }
    
    return clock_sec;
}

// Старые функции удалены - теперь используется audio_render_android

/// Инициализировать AudioClock
void audio_clock_init(AudioClock *c) {
    if (!c) {
        return;
    }
    
    c->clock = NAN;
    c->last_pts = NAN;
    c->last_duration = 0.0;
    c->latency = 0.0;
    c->last_update_us = 0;
    c->valid = 0;
    // Примечание: AudioClock не имеет поля stalled, используем valid=0 для индикации остановки
}

/// Сбросить AudioClock (при seek)
void audio_clock_reset(AudioClock *c) {
    if (!c) {
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.6: SEEK FIX
    // При seek ОБЯЗАТЕЛЬНО:
    // c->clock = NAN;
    // c->last_pts = NAN;
    // c->last_update_us = 0;
    // ❌ audio clock не должен жить между seek
    c->clock = NAN;
    c->last_pts = NAN;
    c->last_duration = 0.0;
    c->last_update_us = 0;
    c->valid = 0;  // Инвалидируем до первого update после seek
    // Примечание: AudioClock не имеет поля stalled, используем valid=0 для индикации остановки
}

// Legacy функция для обратной совместимости
void audio_clock_reset_legacy(AudioState *as, double seek_pos) {
    if (!as) {
        return;
    }
    
    // 🔴 ШАГ K.4: Flush AudioTrack при seek (ОБЯЗАТЕЛЬНО)
    // ⛔ БЕЗ ЭТОГО аудио продолжает старое время после seek
    audio_render_flush(&as->audio_render);
    ALOGI("🔍 SEEK: AudioTrack flushed");
    
    // Используем новую функцию
    audio_clock_reset(&as->clock);
    
    // Обновляем PlayerContext->avsync.audio_clock
    if (as->player_ctx) {
        PlayerContext *ctx = (PlayerContext *)as->player_ctx;
        ctx->avsync.audio_clock = seek_pos;  // Временно устанавливаем на seek_pos
        ctx->avsync.audio_healthy = 0;  // Инвалидируем до первого update
        ctx->avsync.last_audio_clock = seek_pos;
        ctx->avsync.last_audio_clock_ts = 0;
        ALOGI("🔍 SEEK: audio_clock reset to NAN (will be updated from PTS after seek)");
    }
    
    // Legacy поля (для обратной совместимости)
    as->clock_valid = 0;
    as->track_failed = 0;
    as->clock_base_pts = 0.0;
    as->clock_base_time_sec = 0.0;
    as->audio_clock_pts = NAN;
    as->last_audio_clock_pts = NAN;
    
    // Legacy audio_clock (deprecated)
    as->audio_clock = seek_pos;
    
    // Сбрасываем clock для синхронизации (Шаг 20)
    clock_reset(&as->clock, seek_pos);
    
    // Сбрасываем drift correction
    audio_drift_correction_reset(as);
    
    // Сбрасываем samples_written для правильного пересчёта
    as->samples_written = 0;
    as->playback_head_samples = 0;
    
    ALOGI("🔍 SEEK: audio_clock reset (seek mode)");
}

void audio_pause(AudioState *as) {
    if (!as) {
        return;
    }
    
    audio_render_pause(&as->audio_render);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода 7️⃣ AUDIO_PAUSED
    // App pause → AUDIO_PAUSED
    if (as->player_ctx) {
        PlayerContext *ctx = (PlayerContext *)as->player_ctx;
        if (ctx->audio_state == AUDIO_PLAYING) {
            ctx->audio_state = AUDIO_PAUSED;
            ALOGI("🎧 AudioState: AUDIO_PLAYING → AUDIO_PAUSED (app pause)");
            extern void native_player_emit_audio_state_event(const char *state);
            native_player_emit_audio_state_event("paused");
        }
    }
}

void audio_resume(AudioState *as) {
    if (!as) {
        return;
    }
    
    audio_render_start(&as->audio_render);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - точка перехода AUDIO_PAUSED → AUDIO_PLAYING
    // App resume → AUDIO_PLAYING (будет подтверждён при росте playbackHead)
    if (as->player_ctx) {
        PlayerContext *ctx = (PlayerContext *)as->player_ctx;
        if (ctx->audio_state == AUDIO_PAUSED) {
            // Переход в AUDIO_INITIALIZED, затем в AUDIO_PLAYING при росте playbackHead
            ctx->audio_state = AUDIO_INITIALIZED;
            ALOGI("🎧 AudioState: AUDIO_PAUSED → AUDIO_INITIALIZED (app resume, waiting for playbackHead)");
        }
    }
}

bool audio_queue_empty(AudioState *as) {
    if (!as || !as->frameQueue) {
        return true;
    }
    
    // TODO: проверить размер очереди через frame_queue_size()
    // Пока заглушка
    return false;
}

// === Audio Latency & Drift Correction (ШАГ 5) ===

#define AUDIO_DIFF_AVG_NB 20
#define AV_SYNC_THRESHOLD 0.04   // 40 ms (ШАГ 5.4)
#define AV_NO_SYNC_THRESHOLD 0.1 // 100 ms (ШАГ 5.4)
#define MAX_CORRECTION_PERCENT 0.005 // ±0.5% (ШАГ 5.5)

// Вспомогательная функция clamp
static double clamp_double(double value, double min, double max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

void audio_drift_correction_init(AudioState *as) {
    if (!as) {
        return;
    }
    
    as->audio_diff_avg = 0.0;
    as->audio_diff_cum = 0.0; // ШАГ 5.3: Накопительная сумма
    as->audio_diff_count = 0;  // ШАГ 5.3: Счётчик
    as->audio_diff_avg_coef = 0.95; // Экспоненциальное усреднение
    as->audio_diff_threshold = AV_SYNC_THRESHOLD;
    as->audio_no_sync_threshold = AV_NO_SYNC_THRESHOLD;
    as->wanted_nb_samples = 0;
    as->target_sample_rate = as->sample_rate; // ШАГ 5.5: Инициализируем как sample_rate
    as->audio_latency_ms = 0;
}

int audio_drift_correction_apply(AudioState *as, int nb_samples) {
    if (!as) {
        return nb_samples;
    }
    
    // ШАГ 5.4: Решаем, нужна ли коррекция
    if (fabs(as->audio_diff_avg) < as->audio_diff_threshold) {
        // Всё хорошо - коррекция не нужна
        as->wanted_nb_samples = nb_samples;
        as->target_sample_rate = as->sample_rate;
        
        // Сбрасываем compensation если был установлен
        if (as->swr) {
            swr_set_compensation(as->swr, 0, as->sample_rate);
        }
        return nb_samples;
    }
    
    if (fabs(as->audio_diff_avg) >= as->audio_no_sync_threshold) {
        // Дрейф слишком большой - не корректируем (может быть seek/pause)
        as->wanted_nb_samples = nb_samples;
        as->target_sample_rate = as->sample_rate;
        return nb_samples;
    }
    
    // ШАГ 5.5: Мягкая коррекция (максимум ±0.5%)
    double correction = clamp_double(as->audio_diff_avg * 0.1, -MAX_CORRECTION_PERCENT, MAX_CORRECTION_PERCENT);
    as->target_sample_rate = as->sample_rate * (1.0 - correction);
    
    // Вычисляем коррекцию количества сэмплов
    // wanted_nb_samples = nb_samples + (int)(diff * sample_rate)
    double correction_samples = as->audio_diff_avg * as->sample_rate;
    as->wanted_nb_samples = nb_samples + (int)correction_samples;
    
    // Ограничиваем коррекцию (не более ±0.5%)
    int max_correction = (int)(nb_samples * MAX_CORRECTION_PERCENT);
    if (as->wanted_nb_samples > nb_samples + max_correction) {
        as->wanted_nb_samples = nb_samples + max_correction;
    } else if (as->wanted_nb_samples < nb_samples - max_correction) {
        as->wanted_nb_samples = nb_samples - max_correction;
    }
    
    // ШАГ 5.6: Применяем time stretch через swr_set_compensation
    // pitch не меняется, только количество сэмплов
    if (as->swr) {
        int compensation = (int)(as->audio_diff_avg * as->sample_rate);
        swr_set_compensation(as->swr, compensation, as->sample_rate);
    }
    
    return as->wanted_nb_samples;
}

void audio_drift_correction_update(AudioState *as, double drift) {
    if (!as) {
        return;
    }
    
    // ШАГ 5.3: Усредняем diff (anti-jitter)
    // Используем два метода: накопительное и экспоненциальное
    as->audio_diff_cum += drift;
    as->audio_diff_count++;
    
    if (as->audio_diff_count >= AUDIO_DIFF_AVG_NB) {
        // Накопительное усреднение (ШАГ 5.3)
        as->audio_diff_avg = as->audio_diff_cum / as->audio_diff_count;
        as->audio_diff_cum = 0.0;
        as->audio_diff_count = 0;
    } else {
        // Экспоненциальное усреднение (для плавности)
        as->audio_diff_avg = as->audio_diff_avg * as->audio_diff_avg_coef + 
                             drift * (1.0 - as->audio_diff_avg_coef);
    }
    
    // Ограничиваем максимальный дрейф (±100ms)
    if (as->audio_diff_avg > 0.1) {
        as->audio_diff_avg = 0.1;
    } else if (as->audio_diff_avg < -0.1) {
        as->audio_diff_avg = -0.1;
    }
}

void audio_drift_correction_reset(AudioState *as) {
    if (!as) {
        return;
    }
    
    // Сбрасываем дрейф (при seek/resume)
    as->audio_diff_avg = 0.0;
    as->audio_diff_cum = 0.0; // ШАГ 5.3: Сброс накопительной суммы
    as->audio_diff_count = 0;  // ШАГ 5.3: Сброс счётчика
    as->wanted_nb_samples = 0;
    as->target_sample_rate = as->sample_rate; // ШАГ 5.5: Сброс target rate
    
    // Сбрасываем compensation в swr
    if (as->swr) {
        swr_set_compensation(as->swr, 0, as->sample_rate);
    }
}

int audio_get_latency(AudioState *as, JNIEnv *env) {
    if (!as) {
        return 0;
    }
    
    // TODO: Реализовать через audio_render_android
    // Вызвать AudioTrack.getLatency() через JNI
    // Пока возвращаем сохранённое значение
    return as->audio_latency_ms;
}

// === 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC IMPLEMENTATION - ШАГ 4 ===

/// Получить monotonic time в миллисекундах
static inline uint64_t now_ms(void) {
    return av_gettime() / 1000;  // микросекунды → миллисекунды
}

// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.1: Audio clock fix
// ✅ Audio clock = AudioTrack.getPlaybackHeadPosition()
// ❌ PTS больше НЕ используется как master clock
// 📌 PTS используется только для начальной синхронизации, затем playbackHead = источник правды

/// Проверить audio stall (Huawei / HiSilicon case)
///
/// @param as Состояние аудио
void audio_check_stall(AudioState *as) {
    if (!as || !as->clock.valid) {
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем last_update_us (микросекунды)
    int64_t now_us = av_gettime_relative();
    double dt = (double)(now_us - as->clock.last_update_us) / 1000000.0;  // microseconds → seconds
    if (dt > 0.5) {  // 500ms без обновления → stall
        ALOGW("🚨 AudioClock: STALL detected (no update for %.3f sec)", dt);
        
        // Эмитим событие (будет добавлено в native_player_jni.c)
        // native_player_emit_diagnostic_event("AUDIO_STALLED");
    }
}

/// Попытаться восстановить AudioTrack после stall (one-shot recovery)
///
/// @param as Состояние аудио
void audio_try_recover(AudioState *as) {
    if (!as || !as->audio_render.audio_track) {
        return;
    }
    
    ALOGI("🔁 AudioClock: Attempting recovery from stall");
    
    // Stop → Flush → Play (one-shot recovery)
    audio_render_stop(&as->audio_render);
    audio_render_flush(&as->audio_render);
    audio_render_start(&as->audio_render);
    
    // Сбрасываем last_update_us
    as->clock.last_update_us = av_gettime_relative();
    
    ALOGI("✅ AudioClock: Recovery attempt complete");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.5: Audio stall detection
///
/// @param c AudioClock
/// @return 1 если stalled, 0 если running
int audio_clock_is_stalled(AudioClock *c) {
    if (!c || !c->valid) {
        return 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем last_update_us (микросекунды)
    int64_t now_us = av_gettime_relative();
    double dt = (double)(now_us - c->last_update_us) / 1000000.0;  // microseconds → seconds
    return dt > 0.5; // 500ms
}

// === 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC IMPLEMENTATION - ШАГ 4: ASSERT-ы ===

/// Проверить ASSERT-ы для audio clock (обязательные)
///
/// ASSERT(audio_clock >= last_audio_clock - 0.001);
/// ASSERT(!(master == AUDIO && !audio_valid));
void audio_clock_assert(AudioState *as, void *ctx_ptr) {
    PlayerContext *ctx = (PlayerContext *)ctx_ptr;
    if (!as || !ctx) {
        return;
    }
    
    #ifdef DEBUG
    // 1. ASSERT(!isnan(audio_clock))
    if (as->clock.valid && isnan(as->clock.clock)) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock is NAN (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    
    // 2. ASSERT(audio_clock >= 0)
    if (as->clock.valid && as->clock.clock < 0.0) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock < 0 (%.3f) (FATAL)", as->clock.clock);
        abort(); // 🔥 FATAL в debug
    }
    
    // 3. ASSERT(audio_clock monotonic)
    static double last_audio_clock = 0.0;
    if (as->clock.valid && !isnan(as->clock.clock) && as->clock.clock < last_audio_clock - 0.001) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock regression (%.3f < %.3f) (FATAL)", 
              as->clock.clock, last_audio_clock);
        abort(); // 🔥 FATAL в debug
    }
    if (as->clock.valid && !isnan(as->clock.clock)) {
        last_audio_clock = as->clock.clock;
    }
    
    // 4. Нельзя быть audio-master без валидного аудио
    if (ctx->avsync.master == CLOCK_MASTER_AUDIO) {
        bool audio_valid = as->clock.valid && !audio_clock_is_stalled(&as->clock);
        if (!audio_valid) {
            ALOGE("❌ AVSYNC_ASSERT FAILED: master=AUDIO but audio invalid (FATAL)");
            abort(); // 🔥 FATAL в debug
        }
    }
    #endif
}

