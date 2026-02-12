#pragma once

#include <stdbool.h>
#include "ffmpeg_player.h"  // для PlayerContext, AudioState

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-MASTER - архитектурный контракт
///
/// AVSYNC всегда подчиняется реально воспроизводимому устройству вывода:
/// - Audio DAC (если audioState == PLAYING)
/// - VSYNC дисплея (если video-only)
///
/// Не декодеру, не clock'у, не PTS.

/// Тип master clock
typedef enum {
    AVSYNC_MASTER_NONE,      // Нет master (paused, seeking)
    AVSYNC_MASTER_AUDIO,     // Audio MASTER (audioState == PLAYING)
    AVSYNC_MASTER_VIDEO,     // Video MASTER (video-only режим)
} AvSyncMasterType;

/// Статус валидности master clock
typedef struct {
    AvSyncMasterType type;           // Тип master clock
    bool is_valid;                   // Master clock валиден?
    double clock_value;              // Текущее значение master clock (секунды)
    double last_update_time;         // Время последнего обновления (секунды)
    bool audio_clock_valid;          // Audio clock валиден?
    bool video_clock_valid;          // Video clock валиден?
} AvSyncMasterStatus;

/// 🔥 КРИТИЧЕСКИЙ FIX: Определить master clock согласно AVSYNC-MASTER контракту
///
/// Правила:
/// M1. Если hasAudio == true AND audioState == PLAYING → Audio MASTER
/// M2. Если hasAudio == false → Video MASTER
/// M3. Если hasAudio == true AND audioState != PLAYING AND videoState == PLAYING → FATAL
///
/// @param ctx PlayerContext
/// @return AvSyncMasterStatus
AvSyncMasterStatus avsync_master_determine(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: Проверить валидность master clock
///
/// Master clock валиден ТОЛЬКО если:
/// - Audio MASTER: audioState == PLAYING && AudioTrack.playState == PLAYING && noAudioException
/// - Video MASTER: eglSwapBuffers выполнен && frame реально показан && есть VSYNC timestamp
///
/// @param ctx PlayerContext
/// @param master_status AvSyncMasterStatus (из avsync_master_determine)
/// @return true если master clock валиден
bool avsync_master_is_valid(PlayerContext *ctx, const AvSyncMasterStatus *master_status);

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC GATE - проверка, разрешена ли AVSYNC операция
///
/// AVSYNC разрешён ТОЛЬКО если masterClock.isValid == true
/// Иначе:
/// - ❌ запрещены sleep / delay
/// - ❌ запрещён drift correction
/// - ❌ запрещён frame scheduling
/// - ✅ разрешён только WAIT
///
/// @param ctx PlayerContext
/// @return true если AVSYNC GATE открыт
bool avsync_master_gate_is_open(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: Проверить FATAL условия
///
/// F1. Audio master потерян: audioState == PLAYING → audio exception → audioState != PLAYING → video still playing
/// F2. Clock stall: masterClock not advanced > 500ms
/// F3. Drift runaway: drift > 1s for > N frames
///
/// @param ctx PlayerContext
/// @param master_status AvSyncMasterStatus
/// @return true если обнаружено FATAL условие
bool avsync_check_fatal_conditions(PlayerContext *ctx, const AvSyncMasterStatus *master_status);

/// 🔥 КРИТИЧЕСКИЙ FIX: Получить текущее значение master clock
///
/// @param ctx PlayerContext
/// @param master_status AvSyncMasterStatus
/// @return Значение master clock в секундах (или 0.0 если невалиден)
double avsync_master_get_clock(PlayerContext *ctx, const AvSyncMasterStatus *master_status);

/// 🔥 КРИТИЧЕСКИЙ FIX: Вычислить drift между audio и video
///
/// Если Audio MASTER: drift = video_pts - audio_pts
/// Если Video MASTER: drift = audio_pts - video_pts
///
/// @param ctx PlayerContext
/// @param master_status AvSyncMasterStatus
/// @param video_pts Video PTS (секунды)
/// @return Drift в секундах (положительный = video/audio ahead, отрицательный = behind)
double avsync_compute_drift(PlayerContext *ctx, const AvSyncMasterStatus *master_status, double video_pts);

