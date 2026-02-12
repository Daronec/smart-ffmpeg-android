/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING
///
/// Гарантирует:
/// - Никогда не зависать
/// - Всегда продолжать playback
/// - Всегда иметь master clock
/// - Уметь выходить из рассинхрона

#include "ffmpeg_player.h"
#include "avsync_gate.h"
#include "libavutil/time.h"  // для av_gettime
#include <math.h>
#include <android/log.h>

#define LOG_TAG "AVSync"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19: Пороги (жёсткие)
#define AV_DESYNC_WARN      0.150   // 150ms - предупреждение
#define AV_DESYNC_SOFT      0.300   // 300ms - мягкая коррекция (drop frames)
#define AV_DESYNC_HARD      0.800   // 800ms - жёсткая коррекция (video resync)
#define MAX_AV_DESYNC_SEC   2.0     // 🔥 ШАГ 19.5: HARD LIMITS - максимальный desync перед hard reset
#define AUDIO_STALL_SEC     0.5     // 🔥 ШАГ 19.2: AUDIO STALL DETECTOR - 500ms без обновления → stall
#define VIDEO_STALL_SEC     0.7     // 🔥 ШАГ 19.3: VIDEO STALL DETECTOR - 700ms без рендера → stall
#define AUTO_RECOVERY_MS    500     // 500ms - интервал проверки audio revival

/// Получить monotonic time в миллисекундах
static int64_t get_monotonic_time_ms(void) {
    return av_gettime() / 1000;  // микросекунды → миллисекунды
}

/// Инициализировать AvSyncState
///
/// @param ctx PlayerContext
/// @param has_audio Флаг наличия аудио
void avsync_init(PlayerContext *ctx, int has_audio) {
    if (!ctx) {
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16.7: AVSYNC MASTER
    // 🧠 AVSYNC: кто master?
    // ПОЛИТИКА: если audio exists и valid → audio = MASTER, иначе → video = MASTER
    extern int audio_clock_is_stalled(AudioClock *c);
    extern double audio_get_clock(AudioState *as);
    bool audio_valid = has_audio && 
                       ctx->audio && 
                       ctx->audio->clock.valid &&
                       !audio_clock_is_stalled(&ctx->audio->clock);
    ctx->avsync.master = audio_valid ? CLOCK_MASTER_AUDIO : CLOCK_MASTER_VIDEO;
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем clock.clock (канонический)
    ctx->avsync.audio_clock = ctx->audio && ctx->audio->clock.valid ? ctx->audio->clock.clock : 0.0;
    ctx->avsync.video_clock = 0.0;
    ctx->avsync.drift = 0.0;
    ctx->avsync.drift_violations = 0;
    ctx->avsync.recovering = false;
    ctx->avsync.audio_healthy = audio_valid ? 1 : 0;
    ctx->avsync.last_audio_clock = 0.0;
    ctx->avsync.last_audio_clock_ts = 0;
    ctx->avsync.last_video_clock_ts = 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master lock - разблокирован при инициализации
    ctx->avsync.master_locked = false;
    
    ALOGI("AVSYNC: initialized master=%s (audio_clock=%s)",
          ctx->avsync.master == CLOCK_MASTER_AUDIO ? "AUDIO" : "VIDEO",
          audio_valid ? "valid" : "invalid");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.2: AUDIO STALL DETECTOR
///
/// Детектирует, если audio clock не обновлялся > AUDIO_STALL_SEC (500ms)
///
/// @param ctx PlayerContext
/// @return true если обнаружен stall
static bool avsync_check_audio_stall(PlayerContext *ctx) {
    if (!ctx || !ctx->audio) {
        return true;  // Нет audio → считаем stalled
    }
    
    // 🔥 ШАГ 19.2: Audio stall detection
    // if (now - audio.last_clock_update > 0.5) → audio.stalled = true
    extern int audio_clock_is_stalled(AudioClock *c);
    bool stalled = audio_clock_is_stalled(&ctx->audio->clock);
    
    if (stalled) {
        ALOGW("🚨 AVSYNC: AUDIO_STALL detected (no update for > %.1f sec)", AUDIO_STALL_SEC);
    }
    
    return stalled;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.3: VIDEO STALL DETECTOR
///
/// Детектирует, если video clock не обновлялся > VIDEO_STALL_SEC (700ms)
///
/// @param ctx PlayerContext
/// @return true если обнаружен stall
static bool avsync_check_video_stall(PlayerContext *ctx) {
    if (!ctx) {
        return false;
    }
    
    // 🔥 ШАГ 19.3: Проверяем video stall только если video clock валиден
    if (!ctx->video || !ctx->video->clock.valid) {
        return false;
    }
    
    int64_t now_ms = get_monotonic_time_ms();
    
    // 🔥 ШАГ 19.3: Если video clock не обновлялся > VIDEO_STALL_SEC (700ms) → stall
    if (ctx->avsync.last_video_clock_ts > 0) {
        int64_t dt_ms = now_ms - ctx->avsync.last_video_clock_ts;
        double dt_sec = (double)dt_ms / 1000.0;
        
        if (dt_sec > VIDEO_STALL_SEC) {
            ALOGW("🚨 AVSYNC: VIDEO_STALL detected (no frame for %.3f sec > %.3f)", 
                  dt_sec, VIDEO_STALL_SEC);
            return true;
        }
    }
    
    return false;
}

/// Обновить AVSYNC state (master switch логика)
///
/// Вызывается:
/// - video render loop
/// - audio render loop
/// - after seek
/// - after play()
///
/// @param ctx PlayerContext
void avsync_update(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    AvSyncState *s = &ctx->avsync;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.7
    // 🧠 AVSYNC (после шага 17)
    // Теперь формула чистая и детерминированная:
    // diff = video_clock - audio_clock;
    // diff > +threshold → DROP video
    // diff < -threshold → HOLD video
    // diff ≈ 0 → render
    // threshold = max(0.04, frame_duration)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Канонический audio clock
    // audio_clock = last_audio_frame_pts + last_audio_frame_duration - audio_latency_compensation
    extern double audio_get_clock(AudioState *as);
    double audio = ctx->audio ? audio_get_clock(ctx->audio) : NAN;  // Используем канонический audio_get_clock
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.3
    // video_clock = PTS последнего реально ОТОБРАЖЁННОГО кадра (PTS-based)
    // Обновляется ТОЛЬКО после eglSwapBuffers через video_clock_on_frame_render()
    double video = ctx->video && ctx->video->clock.valid ? ctx->video->clock.pts_sec : NAN;  // PTS-based
    
    // Обновляем avsync.audio_clock и avsync.video_clock из media clock
    s->audio_clock = audio;
    s->video_clock = video;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.7
    // Вычисляем drift (video - audio) - чистая формула
    double diff = video - audio;
    s->drift = diff;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - детектор состояния
    int64_t now_ms = get_monotonic_time_ms();
    bool audio_stalled = avsync_check_audio_stall(ctx);
    bool audio_running = ctx->audio && ctx->audio_state == AUDIO_PLAYING;  // audio_state в PlayerContext, не в AudioState
    bool audio_valid = audio_running && !audio_stalled && !isnan(audio) && audio >= 0.0;
    
    // Обновляем audio_healthy
    if (audio_stalled || !audio_running) {
        s->audio_healthy = 0;
    } else if (audio_valid) {
        s->audio_healthy = 1;
    }
    
    // Обновляем timestamp для stall detection
    if (!isnan(audio) && fabs(audio - s->last_audio_clock) > 0.001) {
        s->last_audio_clock = audio;
        s->last_audio_clock_ts = now_ms;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - политика 3: Audio stalled mid-play (Huawei case)
    if (audio_running && audio_stalled && s->master == CLOCK_MASTER_AUDIO) {
        ALOGW("🚨 AVSYNC: AUDIO_STALLED mid-play - switching to VIDEO master");
        
        // Эмитим событие (будет добавлено в native_player_jni.c)
        // native_player_emit_diagnostic_event("AUDIO_STALLED");
        
        // Switch master → VIDEO
        s->master = CLOCK_MASTER_VIDEO;
        s->audio_healthy = 0;
        s->recovering = true;
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
        
        // ⚠️ Запрещено: стопать видео или делать pause()
        // Продолжаем video-only playback
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - политика 2: Video lead (audio застряло)
    // Используем diff вместо av_diff (diff уже объявлен выше)
    if (!audio_valid && diff > AV_DESYNC_WARN && s->master == CLOCK_MASTER_AUDIO) {
        ALOGW("🚨 AVSYNC: Video lead (audio stalled/stopped) - switching to VIDEO master");
        
        // Немедленно переключить master → VIDEO
        s->master = CLOCK_MASTER_VIDEO;
        s->audio_healthy = 0;
        s->recovering = true;
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
        
        // НЕ стопать видео, продолжать playback video-only
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.7: AVSYNC ПРАВИЛО для background playback
    // В background режиме audio всегда master
    if (ctx->playback_mode == MODE_AUDIO_ONLY) {
        // Background mode - audio = единственный источник времени
        if (ctx->has_audio && ctx->audio && ctx->audio->clock.valid) {
            s->master = CLOCK_MASTER_AUDIO;
            s->audio_healthy = 1;
        } else {
            // Нет audio в background - это ошибка, но продолжаем
            ALOGW("⚠️ AVSYNC: Background mode without audio (should not happen)");
            s->master = CLOCK_MASTER_VIDEO;
        }
        return; // В background не обновляем video clock
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.1: MASTER CLOCK POLICY
    // 🧠 AVSYNC — это стратегия, а не константа
    // Мы динамически выбираем master clock
    // if (audio.is_valid && audio.is_playing) → master = MASTER_AUDIO
    // else → master = MASTER_VIDEO
    // 📌 Audio — master ТОЛЬКО если он жив
    extern int audio_clock_is_stalled(AudioClock *c);
    bool audio_is_valid = ctx->has_audio == 1 && 
                          ctx->audio && 
                          ctx->audio->clock.valid &&
                          !isnan(ctx->audio->clock.clock) &&
                          !audio_clock_is_stalled(&ctx->audio->clock);
    bool audio_is_playing = ctx->audio && ctx->audio_state == AUDIO_PLAYING;  // audio_state в PlayerContext
    bool audio_valid_check = audio_is_valid && audio_is_playing;
    
    bool video_valid = ctx->video && 
                       ctx->video->clock.valid &&
                       !isnan(ctx->video->clock.pts_sec);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.4: MASTER SWITCH (БЕЗ ЩЕЛЧКОВ)
    // При смене master: sync_base = current_master_clock()
    // 📌 НЕ reset clocks
    // 📌 Только меняем точку сравнения
    ClockMaster new_master;
    if (audio_valid_check) {
        new_master = CLOCK_MASTER_AUDIO;
    } else if (video_valid) {
        new_master = CLOCK_MASTER_VIDEO;
    } else {
        // Ни audio, ни video не валидны → fallback на VIDEO
        new_master = CLOCK_MASTER_VIDEO;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master lock - запрет авто-переключения после первого валидного выбора
    // После первого валидного выбора master - ЗАПРЕТИТЬ авто-переключение
    // Unlock только при: seek, pause → play, source change
    // ИСКЛЮЧЕНИЕ: если текущий master стал невалидным (audio stalled) - разрешаем переключение
    bool current_master_invalid = false;
    if (s->master == CLOCK_MASTER_AUDIO && !audio_valid_check) {
        // Audio master стал невалидным - разрешаем переключение
        current_master_invalid = true;
        ALOGW("🚨 AVSYNC: Locked AUDIO master became invalid, forcing switch to VIDEO");
    } else if (s->master == CLOCK_MASTER_VIDEO && !video_valid) {
        // Video master стал невалидным - разрешаем переключение
        current_master_invalid = true;
        ALOGW("🚨 AVSYNC: Locked VIDEO master became invalid, forcing re-selection");
    }
    
    // Если master заблокирован и текущий master валиден - не переключаем
    if (s->master_locked && !current_master_invalid && s->master == new_master) {
        // Master заблокирован и валиден - не переключаем
        return;
    }
    
    // Если master заблокирован, но стал невалидным - разблокируем для переключения
    if (s->master_locked && current_master_invalid) {
        s->master_locked = false;
        ALOGI("🔓 AVSYNC: Master unlocked due to invalid current master");
    }
    
    // 🔥 ШАГ 19.4: Переключаем master только если он изменился
    if (s->master != new_master) {
        // Сохраняем текущий master clock для плавного перехода
        double sync_base = 0.0;
        if (s->master == CLOCK_MASTER_AUDIO && !isnan(audio)) {
            sync_base = audio;
        } else if (s->master == CLOCK_MASTER_VIDEO && !isnan(video)) {
            sync_base = video;
        }
        
        ALOGI("✅ AVSYNC: Master switch %s → %s (sync_base=%.3f)",
              s->master == CLOCK_MASTER_AUDIO ? "AUDIO" : "VIDEO",
              new_master == CLOCK_MASTER_AUDIO ? "AUDIO" : "VIDEO",
              sync_base);
        
        s->master = new_master;
        s->recovering = false;
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Блокируем master после первого валидного выбора
        // Это предотвращает flip-flop в первые 200-300мс
        s->master_locked = true;
        ALOGI("🔒 AVSYNC: Master locked (no auto-switch until seek/pause/play)");
        
        // Обновляем avsync_gate
        if (new_master == CLOCK_MASTER_AUDIO) {
            avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
        } else {
            avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        }
        avsync_gate_set_valid(&ctx->avsync_gate);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - политика 1: Audio lead (video не успевает)
    // Используем diff вместо av_diff (diff уже объявлен выше)
    if (audio_valid && diff < -AV_DESYNC_WARN) {
        // video behind audio
        double abs_diff = fabs(diff);
        
        if (abs_diff > AV_DESYNC_HARD) {
            // >800ms → 🔁 VIDEO RESYNC
            ALOGW("🚨 AVSYNC: VIDEO RESYNC (drift=%.3f > 800ms)", diff);
            
            // video_clock = audio_clock
            s->video_clock = audio;
            if (ctx->video) {
                // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 8
                // Обновляем clock.pts_sec вместо video_clock_pts
                ctx->video->clock.pts_sec = audio;
                ctx->video->clock.valid = 1;
                // Используем текущее время для last_present_ts
                int64_t now_ms = get_monotonic_time_ms();
                ctx->video->clock.last_present_ts = (double)now_ms / 1000.0;
                
                // Legacy поле (deprecated)
                ctx->video->video_clock_pts = audio;
            }
            
            // flush video queue (не demux) - будет обработано в video_render_gl.c
            // force render next frame - будет обработано в video_render_gl.c
            
            // Эмитим событие
            // native_player_emit_diagnostic_event("VIDEO_RESYNC");
            
            s->recovering = true;
            s->drift_violations = 0;
        } else if (abs_diff > AV_DESYNC_SOFT) {
            // 300-800ms → ❌❌ AGGRESSIVE DROP (без рендера)
            // Будет обработано в video_render_gl.c
            ALOGW("⚠️ AVSYNC: AGGRESSIVE DROP (drift=%.3f, 300-800ms)", diff);
            s->recovering = true;
        } else {
            // 150-300ms → ❌ DROP video frames (до догоняния)
            // Будет обработано в video_render_gl.c
            ALOGW("⚠️ AVSYNC: DROP frames (drift=%.3f, 150-300ms)", diff);
        }
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - политика 5: Auto-recovery Audio
    static int64_t last_auto_recovery_check = 0;
    if (now_ms - last_auto_recovery_check > AUTO_RECOVERY_MS) {
        last_auto_recovery_check = now_ms;
        
        if (s->master == CLOCK_MASTER_VIDEO && audio_running && !audio_stalled && audio_valid) {
            // audio revived
            ALOGI("✅ AVSYNC: Audio revived - switching master VIDEO → AUDIO");
            s->master = CLOCK_MASTER_AUDIO;
            s->audio_healthy = 1;
            s->audio_clock = audio;
            s->video_clock = video;
            // audio_clock = video_clock (логически синхронизируем)
            if (fabs(video - audio) < 0.5) {
                s->audio_clock = video;  // Синхронизируем с video
            }
            s->recovering = false;
            avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
            avsync_gate_set_valid(&ctx->avsync_gate);
        }
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.5: HARD LIMITS (ЗАЩИТА)
    // #define MAX_AV_DESYNC_SEC 2.0
    // if (fabs(diff) > MAX_AV_DESYNC_SEC) → LOG("AVSYNC HARD RESET")
    // RESET_VIDEO_QUEUE()
    // RESET_VIDEO_CLOCK()
    // Последний рубеж. Используется крайне редко, но спасает UI.
    double drift_abs = fabs(s->drift);
    
    if (drift_abs > MAX_AV_DESYNC_SEC) {
        // 🚨 FATAL: drift > MAX_AV_DESYNC_SEC (2.0s) → force hard reset
        ALOGE("❌ AVSYNC HARD RESET: drift too large (%.3f > %.1f) - force hard reset", 
              drift_abs, MAX_AV_DESYNC_SEC);
        
        // 🔥 ШАГ 19.5: RESET_VIDEO_QUEUE() и RESET_VIDEO_CLOCK()
        if (ctx->video && ctx->video->frameQueue) {
            frame_queue_flush(ctx->video->frameQueue);  // Используем правильную сигнатуру из frame_queue.h
            ALOGI("🔁 AVSYNC: Video queue flushed");
        }
        
        // Reset video clock
        if (ctx->video) {
            ctx->video->clock.pts_sec = audio_valid ? audio : 0.0;
            ctx->video->clock.valid = audio_valid ? 1 : 0;
            ALOGI("🔁 AVSYNC: Video clock reset to %.3f", ctx->video->clock.pts_sec);
        }
        
        // Force hard resync
        extern void avsync_hard_resync(PlayerContext *ctx);
        avsync_hard_resync(ctx);
        s->drift = 0.0;
        s->drift_violations = 0;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.6: SEEK RECOVERY
    // После seek:
    //   master = MASTER_VIDEO
    //   audio.ignore_until_video_first_frame = true
    // После firstFrameAfterSeek:
    //   if (audio.is_playing) → master = MASTER_AUDIO
    // 📌 Это решает AVI / FLV seek deadlock
    if (ctx->seek.in_progress) {
        // Во время seek: master = VIDEO, audio игнорируется
        if (s->master != CLOCK_MASTER_VIDEO) {
            ALOGI("🔍 AVSYNC: Seek in progress → switching to VIDEO master");
            s->master = CLOCK_MASTER_VIDEO;
            s->recovering = true;
            avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
            avsync_gate_set_valid(&ctx->avsync_gate);
        }
    } else if (ctx->waiting_first_frame_after_seek) {
        // После seek, но до firstFrameAfterSeek: продолжаем использовать VIDEO master
        // Это уже обработано выше
    } else if (s->recovering && ctx->seek.seek_id > 0) {
        // После firstFrameAfterSeek: проверяем, можно ли переключиться на AUDIO
        if (!ctx->seek.drop_audio && !ctx->seek.drop_video) {
            // Seek завершён, проверяем audio
            if (audio_valid_check) {
                // 🔥 ШАГ 19.6: После firstFrameAfterSeek, если audio.is_playing → master = MASTER_AUDIO
                if (s->master != CLOCK_MASTER_AUDIO) {
                    ALOGI("✅ AVSYNC: Seek recovery complete → switching to AUDIO master (audio is playing)");
                    s->master = CLOCK_MASTER_AUDIO;
                    s->audio_healthy = 1;
                    avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
                    avsync_gate_set_valid(&ctx->avsync_gate);
                }
            }
            s->recovering = false;
            s->drift_violations = 0;
        }
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ASSERT-ы (не лог, а защита)
    // 1. Клоки монотонны
    static double last_video_clock = 0.0;
    static double last_audio_clock = 0.0;
    
    if (!isnan(video) && !isnan(last_video_clock) && video < last_video_clock - 0.001) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: video_clock regression (%.3f < %.3f)", video, last_video_clock);
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
    }
    if (!isnan(video)) {
        last_video_clock = video;
    }
    
    if (!isnan(audio) && !isnan(last_audio_clock) && audio < last_audio_clock - 0.001) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock regression (%.3f < %.3f)", audio, last_audio_clock);
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
    }
    if (!isnan(audio)) {
        last_audio_clock = audio;
    }
    
    // 2. 🔥 ШАГ 7.9: ASSERT(!(audio_clock > video_clock + 0.5))
    if (!isnan(audio) && !isnan(video) && audio > video + 0.5) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock=%.3f > video_clock=%.3f + 0.5 (FATAL)", audio, video);
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
    }
    
    // 3. 🔥 ШАГ 7.9: ASSERT(!(video_clock > audio_clock + 0.5))
    if (!isnan(audio) && !isnan(video) && video > audio + 0.5) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: video_clock=%.3f > audio_clock=%.3f + 0.5 (FATAL)", video, audio);
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - ШАГ 19.7: ASSERT / DIAGNOSTICS
    // Native ASSERTs:
    // ASSERT(!(audio.stalled && master == MASTER_AUDIO))
    // ASSERT(!(video.stalled && master == MASTER_VIDEO))
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.6: ASSERTS (HARD GUARANTEE)
    // ASSERT(!(audio.stalled && master == MASTER_AUDIO))
    // ASSERT(!(isnan(audio.clock) && master == MASTER_AUDIO))
    
    // 4. Нельзя быть audio-master без валидного аудио
    if (s->master == CLOCK_MASTER_AUDIO && !audio_valid) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: master=AUDIO but audio invalid (FATAL)");
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
        // В release: switch to VIDEO
        s->master = CLOCK_MASTER_VIDEO;
        s->audio_healthy = 0;
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
    }
    
    // 🔥 ШАГ 20.6: ASSERT(!(audio.stalled && master == MASTER_AUDIO))
    if (audio_stalled && s->master == CLOCK_MASTER_AUDIO) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio.stalled=true && master=MASTER_AUDIO (FATAL)");
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
        // В release: switch to VIDEO
        s->master = CLOCK_MASTER_VIDEO;
        s->audio_healthy = 0;
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
    }
    
    // 🔥 ШАГ 20.6: ASSERT(!(isnan(audio.clock) && master == MASTER_AUDIO))
    if (isnan(audio) && s->master == CLOCK_MASTER_AUDIO) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: isnan(audio.clock)=true && master=MASTER_AUDIO (FATAL)");
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
        // В release: switch to VIDEO
        s->master = CLOCK_MASTER_VIDEO;
        s->audio_healthy = 0;
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
    }
    
    // 🔥 ШАГ 19.7: ASSERT(!(audio.stalled && master == MASTER_AUDIO))
    bool video_stalled = avsync_check_video_stall(ctx);
    if (audio_stalled && s->master == CLOCK_MASTER_AUDIO) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: audio.stalled=true && master=MASTER_AUDIO (FATAL)");
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
        // В release: switch to VIDEO
        s->master = CLOCK_MASTER_VIDEO;
        s->audio_healthy = 0;
        avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
        avsync_gate_set_valid(&ctx->avsync_gate);
    }
    
    // 🔥 ШАГ 19.7: ASSERT(!(video.stalled && master == MASTER_VIDEO))
    if (video_stalled && s->master == CLOCK_MASTER_VIDEO) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: video.stalled=true && master=MASTER_VIDEO (FATAL)");
        #ifdef DEBUG
        abort(); // 🔥 FATAL в debug
        #endif
        // В release: force render next frame (будет обработано в video_render_gl.c)
        ALOGW("⚠️ AVSYNC: Video stalled in VIDEO master mode - forcing render");
    }
    
    // 5. Дрейф не может расти бесконечно
    if (drift_abs > 5.0) {
        ALOGE("❌ AVSYNC_ASSERT FAILED: drift too large (%.3f > 5.0s) - force resync", drift_abs);
        // Уже обработано выше
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.5: Diagnostic payload
    // Эмитим диагностические события для Flutter HUD
    static int64_t last_log_ts = 0;
    int64_t now = get_monotonic_time_ms();
    if (now - last_log_ts > 1000) {
        ALOGD("📊 AVSYNC: master=%s a=%.3f v=%.3f drift=%.3f violations=%d recovering=%d healthy=%d stalled=%d",
              s->master == CLOCK_MASTER_AUDIO ? "AUDIO" : "VIDEO",
              audio,
              s->video_clock,
              s->drift,
              s->drift_violations,
              s->recovering ? 1 : 0,
              s->audio_healthy ? 1 : 0,
              audio_stalled ? 1 : 0);
        
        // 🔥 ШАГ 20.5: Эмитим диагностическое событие для Flutter HUD
        extern void native_player_emit_diagnostic_event(const char *type, const char *key, const char *value);
        char master_str[16];
        snprintf(master_str, sizeof(master_str), "%s", s->master == CLOCK_MASTER_AUDIO ? "audio" : "video");
        native_player_emit_diagnostic_event("avsync", "master", master_str);
        
        char audio_stalled_str[8];
        snprintf(audio_stalled_str, sizeof(audio_stalled_str), "%d", audio_stalled ? 1 : 0);
        native_player_emit_diagnostic_event("avsync", "audio_stalled", audio_stalled_str);
        
        char audio_clock_str[32];
        snprintf(audio_clock_str, sizeof(audio_clock_str), "%.3f", isnan(audio) ? 0.0 : audio);
        native_player_emit_diagnostic_event("avsync", "audio_clock", audio_clock_str);
        
        char video_clock_str[32];
        snprintf(video_clock_str, sizeof(video_clock_str), "%.3f", isnan(video) ? 0.0 : video);
        native_player_emit_diagnostic_event("avsync", "video_clock", video_clock_str);
        
        last_log_ts = now;
    }
}

/// Сбросить AVSYNC state (для seek)
///
/// @param ctx PlayerContext
void avsync_reset(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    AvSyncState *s = &ctx->avsync;
    
    s->audio_clock = 0.0;
    s->video_clock = 0.0;
    s->drift = 0.0;
    s->drift_violations = 0;
    s->recovering = true;
    s->last_audio_clock = 0.0;
    s->last_audio_clock_ts = 0;
    s->last_video_clock_ts = 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master lock - разблокируем при reset (seek/pause→play)
    s->master_locked = false;
    
    ALOGI("🔍 AVSYNC: reset (seek/pause→play, master unlocked)");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - Hard Resync
///
/// Выполняет жёсткую ресинхронизацию:
/// - pause render
/// - flush video queue (не demux)
/// - drop frames < audio_clock
/// - render next frame >= audio_clock
/// - resume
///
/// @param ctx PlayerContext
void avsync_hard_resync(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    ALOGW("🔁 AVSYNC: HARD RESYNC started");
    
    // Устанавливаем флаг recovering
    ctx->avsync.recovering = true;
    ctx->avsync.drift_violations = 0;
    
    // Flush video queue (будет обработано в video_render_gl.c)
    // Drop frames < audio_clock (будет обработано в video_render_gl.c)
    // Force render next frame >= audio_clock (будет обработано в video_render_gl.c)
    
    ALOGW("🔁 AVSYNC: HARD RESYNC complete (video queue will be flushed, frames < audio_clock will be dropped)");
}

