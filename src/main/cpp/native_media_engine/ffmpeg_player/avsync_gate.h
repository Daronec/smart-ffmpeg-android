#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - AVSyncGate
///
/// Один объект. Одна правда.
/// НЕ знает FSM. НЕ знает UI. НЕ знает Flutter.
/// Только master clock и его валидность.

/// Тип master clock
typedef enum {
    AVSYNC_MASTER_NONE_GATE,   // Нет master (paused, seeking, invalid)
    AVSYNC_MASTER_AUDIO_GATE,   // Audio MASTER
    AVSYNC_MASTER_VIDEO_GATE,   // Video MASTER
} AvSyncMasterTypeGate;

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSyncGate - ядро AVSYNC системы
///
/// Гарантирует:
/// - ❌ НИ ОДИН кадр не будет отрисован если master clock невалиден
/// - ❌ НИ ОДИН PTS не будет двигаться если master clock невалиден
/// - ❌ НИ ОДИН sleep не будет выполнен если master clock невалиден
typedef struct {
    // Master clock type (atomic для thread-safety)
    atomic_int master;              // AvSyncMasterTypeGate
    
    // Master clock validity (atomic для thread-safety)
    atomic_bool master_valid;      // true = master clock валиден
    
    // Clock values (atomic для thread-safety)
    atomic_uint_fast64_t audio_clock_us;  // Audio clock в микросекундах (для точности)
    atomic_uint_fast64_t video_clock_us;   // Video clock в микросекундах
    
    // Last master clock advance time (для stall detector)
    atomic_int_fast64_t last_master_advance_us;  // Время последнего обновления master clock (микросекунды)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - seek bypass для устранения deadlock
    // Во время seek AVSYNC временно отключён, render разрешён без ожидания audio_clock
    atomic_bool seek_in_progress;  // true = seek в процессе, AVSYNC bypass
    
    // Invalidation reason (для диагностики)
    char invalidation_reason[128];
} AVSyncGate;

/// Инициализировать AVSyncGate
void avsync_gate_init(AVSyncGate *gate);

/// Инвалидировать AVSYNC gate
///
/// @param gate AVSyncGate
/// @param reason Причина инвалидации (для диагностики)
void avsync_gate_invalidate(AVSyncGate *gate, const char *reason);

/// Проверить, открыт ли AVSYNC gate
///
/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH
/// Во время seek (seek_in_progress=true) gate всегда открыт для bypass AVSYNC
///
/// @param gate AVSyncGate
/// @return true если gate открыт (master clock валиден ИЛИ seek в процессе)
bool avsync_gate_is_open(const AVSyncGate *gate);

/// Установить seek_in_progress флаг
///
/// @param gate AVSyncGate
/// @param in_progress true = seek в процессе, false = seek завершён
void avsync_gate_set_seek_in_progress(AVSyncGate *gate, bool in_progress);

/// Проверить, идёт ли seek
///
/// @param gate AVSyncGate
/// @return true если seek в процессе
bool avsync_gate_is_seek_in_progress(const AVSyncGate *gate);

/// Установить master clock type
///
/// @param gate AVSyncGate
/// @param master_type Тип master clock
void avsync_gate_set_master(AVSyncGate *gate, AvSyncMasterTypeGate master_type);

/// Получить master clock type
///
/// @param gate AVSyncGate
/// @return Тип master clock
AvSyncMasterTypeGate avsync_gate_get_master(const AVSyncGate *gate);

/// Установить master clock как валидный
///
/// @param gate AVSyncGate
void avsync_gate_set_valid(AVSyncGate *gate);

/// Обновить audio clock
///
/// @param gate AVSyncGate
/// @param clock_us Audio clock в микросекундах
void avsync_gate_update_audio_clock(AVSyncGate *gate, int64_t clock_us);

/// Обновить video clock
///
/// @param gate AVSyncGate
/// @param clock_us Video clock в микросекундах
void avsync_gate_update_video_clock(AVSyncGate *gate, int64_t clock_us);

/// Получить audio clock
///
/// @param gate AVSyncGate
/// @return Audio clock в микросекундах
int64_t avsync_gate_get_audio_clock_us(const AVSyncGate *gate);

/// Получить video clock
///
/// @param gate AVSyncGate
/// @return Video clock в микросекундах
int64_t avsync_gate_get_video_clock_us(const AVSyncGate *gate);

/// Получить время последнего обновления master clock
///
/// @param gate AVSyncGate
/// @return Время в микросекундах
int64_t avsync_gate_get_last_advance_us(const AVSyncGate *gate);

/// Проверить clock stall (для watchdog)
///
/// @param gate AVSyncGate
/// @param stall_threshold_us Порог для stall в микросекундах (500ms = 500000us)
/// @return true если обнаружен stall
bool avsync_gate_check_stall(const AVSyncGate *gate, int64_t stall_threshold_us);

