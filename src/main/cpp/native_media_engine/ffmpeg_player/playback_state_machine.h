/// Шаг 30: Unified Playback State Machine
///
/// Единая машина состояний для управления воспроизведением
/// Поддерживает прозрачное переключение между HW/SW backends

#ifndef PLAYBACK_STATE_MACHINE_H
#define PLAYBACK_STATE_MACHINE_H

#include <pthread.h>
#include <stdbool.h>

// Forward declarations (полные определения в соответствующих заголовках)
struct VideoBackendMediaCodec;
struct VideoRenderGL;
// AudioState и PlayerContext определены как typedef struct { ... } в соответствующих заголовках
// Используем просто forward declarations без typedef
struct AudioState;
struct PlayerContext;

/// Состояние воспроизведения (Шаг 30.1)
/// ВАЖНО: Используем отдельный enum, чтобы избежать конфликтов с PlaybackState из ffmpeg_player.h
typedef enum {
    STATE_IDLE,        // Плеер не инициализирован
    STATE_PREPARING,   // Инициализация (demux, codecs)
    STATE_READY,       // Готов к воспроизведению
    STATE_PLAYING,     // Воспроизведение идёт
    STATE_PAUSED,      // Приостановлено
    STATE_SEEKING,     // Выполняется seek
    STATE_BUFFERING,   // Буферизация
    STATE_EOF,         // Конец файла
    STATE_ERROR        // Ошибка
} PlaybackStateMachineState;

/// Контекст воспроизведения (Шаг 30.2)
typedef struct PlaybackContext {
    /// Текущее состояние
    PlaybackStateMachineState state;
    
    /// Использовать HW video decode
    bool use_hw_video;
    
    /// HW decode упал (fallback на SW)
    bool hw_failed;
    
    /// Длительность (секунды)
    double duration;
    
    /// Текущая позиция (секунды)
    double position;
    
    /// Приостановлено
    bool paused;
    
    /// Запрос прерывания
    bool abort_request;
    
    /// Mutex для синхронизации
    pthread_mutex_t mutex;
    
    /// Video backend (HW или SW)
    void *video_backend; // VideoBackendMediaCodec* или VideoRenderGL*
    
    /// Audio state
    struct AudioState *audio_state;
    
    /// Player context (для доступа к demux, etc.)
    struct PlayerContext *player_ctx;
} PlaybackContext;

/// Инициализировать контекст воспроизведения
///
/// @param ctx Контекст
void playback_context_init(PlaybackContext *ctx);

/// Освободить контекст воспроизведения
///
/// @param ctx Контекст
void playback_context_destroy(PlaybackContext *ctx);

/// Установить состояние (Шаг 30.5)
///
/// Проверяет допустимость перехода и уведомляет Flutter
///
/// @param ctx Контекст
/// @param next_state Новое состояние
/// @return true если переход выполнен, false если недопустим
bool playback_set_state(PlaybackContext *ctx, PlaybackStateMachineState next_state);

/// Получить текущее состояние
///
/// @param ctx Контекст
/// @return Текущее состояние
PlaybackStateMachineState playback_get_state(PlaybackContext *ctx);

/// Проверить, можно ли перейти из одного состояния в другое (Шаг 30.4)
///
/// @param from Исходное состояние
/// @param to Целевое состояние
/// @return true если переход допустим
bool playback_can_transition(PlaybackStateMachineState from, PlaybackStateMachineState to);

/// Инициализировать воспроизведение (Шаг 30.6)
///
/// @param ctx Контекст
/// @param use_hw Использовать HW decode
/// @return 0 при успехе, <0 при ошибке
int playback_init(PlaybackContext *ctx, bool use_hw);

/// Начать воспроизведение
///
/// @param ctx Контекст
/// @return 0 при успехе, <0 при ошибке
int playback_play(PlaybackContext *ctx);

/// Приостановить воспроизведение (Шаг 30.7)
///
/// @param ctx Контекст
void playback_pause(PlaybackContext *ctx);

/// Возобновить воспроизведение
///
/// @param ctx Контекст
void playback_resume(PlaybackContext *ctx);

/// Выполнить seek (Шаг 30.8)
///
/// @param ctx Контекст
/// @param pts Позиция в секундах
/// @return 0 при успехе, <0 при ошибке
int playback_seek(PlaybackContext *ctx, double pts);

/// Остановить воспроизведение
///
/// @param ctx Контекст
void playback_stop(PlaybackContext *ctx);

/// Выполнить HW → SW fallback (Шаг 30.9)
///
/// @param ctx Контекст
/// @return 0 при успехе, <0 при ошибке
int playback_fallback_to_software(PlaybackContext *ctx);

/// Обработать EOF (Шаг 30.10)
///
/// @param ctx Контекст
void playback_handle_eof(PlaybackContext *ctx);

/// Обновить позицию воспроизведения
///
/// @param ctx Контекст
/// @param position Новая позиция (секунды)
void playback_update_position(PlaybackContext *ctx, double position);

/// Получить текущую позицию
///
/// @param ctx Контекст
/// @return Позиция в секундах
double playback_get_position(PlaybackContext *ctx);

/// Получить длительность
///
/// @param ctx Контекст
/// @return Длительность в секундах
double playback_get_duration(PlaybackContext *ctx);

/// Уведомить Flutter о изменении состояния
///
/// @param ctx Контекст
/// @param state Новое состояние
void playback_notify_flutter_state(PlaybackContext *ctx, PlaybackStateMachineState state);

#endif // PLAYBACK_STATE_MACHINE_H

