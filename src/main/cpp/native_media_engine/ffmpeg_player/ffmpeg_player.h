#ifndef FFMPEG_PLAYER_H
#define FFMPEG_PLAYER_H

#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>  // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.1: для atomic_int seek_serial

// FFmpeg headers
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "audio_renderer.h"
#include "video_renderer.h"
#include "subtitle_manager.h"  // 🔴 ЗАДАЧА 6: Subtitles API
#include "avsync_gate.h"  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION

// Forward declarations
typedef struct PacketQueue PacketQueue;
typedef struct FrameQueue FrameQueue;
// AudioState и VideoState уже определены в audio_renderer.h и video_renderer.h
// Не переопределяем их здесь, чтобы избежать конфликтов

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - SeekState (единый источник правды)
/// Гарантирует:
/// - НИ ОДИН thread не может блокироваться на другом
/// - Seek работает идемпотентно (хоть 10 раз подряд)
/// - AVI / FLV точно перематываются
/// - AVSYNC не ломается
typedef struct {
    bool in_progress;      // Флаг, что seek выполняется
    bool drop_audio;       // Флаг, что audio decode должен дропать кадры
    bool drop_video;       // Флаг, что video decode должен дропать кадры
    
    int64_t target_ms;     // Целевая позиция в миллисекундах
    int64_t seek_id;       // Монотонный счётчик для идемпотентности (monotonic counter)
} SeekState;

// AudioState enum определен в audio_renderer.h как struct AudioState
// Используем enum значения из audio_renderer.h через forward declaration
// Для доступа к enum значениям нужно включить audio_renderer.h

/// 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - enum для audio_state в PlayerContext
/// Это НЕ struct AudioState (который определён в audio_renderer.h)
/// Это enum для поля audio_state в PlayerContext
typedef enum {
    AUDIO_NO_AUDIO = 0,           // Нет аудио стрима в контейнере
    AUDIO_INITIALIZING = 1,       // 🔥 Audio stream найден, инициализация начата
    AUDIO_INITIALIZED = 2,        // AudioTrack создан, готов к воспроизведению
    AUDIO_READY = 3,              // Buffer primed, первый frame записан
    AUDIO_PLAYING = 4,            // AudioTrack.getPlayState() == PLAYSTATE_PLAYING
    AUDIO_PAUSED = 5,             // App pause
    AUDIO_STOPPED_BY_SYSTEM = 6, // AudioTrack остановлен системой (Huawei/HiSilicon)
    AUDIO_DEAD = 7                // Терминальное состояние (фатальная ошибка)
} AudioStateEnum;

/// Режим воспроизведения
typedef enum {
    PLAYBACK_RUNNING,  // Воспроизведение идёт
    PLAYBACK_EOF,      // Достигнут конец файла
    PLAYBACK_STOPPED   // Остановлено
} PlaybackState;

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.1, 13.1: PlaybackMode (Native enum, НЕ FSM Flutter)
/// Определяет режим воспроизведения для background/foreground/frame step
typedef enum {
    MODE_AV,          // Обычный режим (video + audio)
    MODE_AUDIO_ONLY,  // Background / screen off (только audio)
    MODE_FRAME_STEP,  // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.1: Frame stepping (покадровая навигация)
} PlaybackMode;

// === 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING ===

/// Master clock type для AVSYNC
typedef enum {
    CLOCK_MASTER_AUDIO = 0,  // Audio MASTER
    CLOCK_MASTER_VIDEO = 1,  // Video MASTER (fallback)
} ClockMaster;

/// AVSYNC state (единый источник правды)
///
/// Гарантирует:
/// - Никогда не зависать
/// - Всегда продолжать playback
/// - Всегда иметь master clock
/// - Уметь выходить из рассинхрона
typedef struct {
    ClockMaster master;          // Текущий master clock
    
    double audio_clock;          // Audio clock в секундах (из AudioTrack playback head)
    double video_clock;          // Video clock в секундах (last presented frame PTS)
    
    double drift;                // video - audio (drift в секундах)
    int drift_violations;        // Счётчик нарушений drift (>200ms)
    
    bool recovering;             // Флаг восстановления после stall
    bool audio_healthy;          // Флаг здоровья audio (не stalled)
    
    // Stall detection
    double last_audio_clock;     // Последний audio clock для stall detection
    int64_t last_audio_clock_ts; // Timestamp последнего обновления audio clock (ms)
    int64_t last_video_clock_ts; // Timestamp последнего обновления video clock (ms)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Master lock (запрет авто-переключения после первого валидного выбора)
    // После первого валидного выбора master - ЗАПРЕТИТЬ авто-переключение
    // Unlock только при: seek, pause → play, source change
    bool master_locked;           // Флаг блокировки master (после первого валидного выбора)
} AvSyncState;

/// Запрос seek (Шаг 38.2)
typedef struct SeekRequest {
    /// Целевой PTS (в stream time_base)
    int64_t target_pts;
    
    /// PTS keyframe, на который был выполнен fast seek
    int64_t seek_start_pts;
    
    /// Точный seek (decode & drop до target_pts)
    bool exact;
    
    /// Флаг очистки пайплайна (flush queues)
    bool flushing;
    
    /// Флаг, что seek выполняется
    bool seeking;
} SeekRequest;

/// Глобальное состояние плеера (для seek и EOF)
typedef struct {
    /// Запрос seek (Шаг 38.2)
    SeekRequest seek_req;
    
    /// Целевая позиция для seek (в секундах) - для обратной совместимости
    double seek_pos;
    
    /// Флаг запроса seek (legacy, для обратной совместимости)
    int seek_req_legacy;
    
    /// Флаги для avformat_seek_file
    int seek_flags;
    
    /// Флаг прерывания
    int abort_request;
    
    /// Mutex для seek операций
    pthread_mutex_t seek_mutex;
    
    /// Флаги завершения потоков (для EOF)
    int audio_finished;
    int video_finished;
    
    /// Состояние воспроизведения
    PlaybackState state;
    
    /// Режим повтора (0=off, 1=one, 2=all)
    int repeat_mode;

    /// Параметры воспроизведения (для скорости)
    struct {
        double speed;  // Скорость воспроизведения (0.5 .. 3.0)
    } playback;
} PlayerState;

/// Контекст плеера
///
/// Содержит все состояние FFmpeg плеера:
/// - Format context (демux)
/// - Codec contexts (video/audio декодеры)
/// - Очереди пакетов и кадров
/// - Clock для синхронизации
/// - Флаги состояния
/// - Seek состояние
typedef struct {
    // Format context
    AVFormatContext *fmt;
    
    // Stream indices
    int videoStream;
    int audioStream;
    
    // 🔥 КРИТИЧНО: Явный флаг наличия аудио (для video-only режима)
    // Устанавливается в open_media() после определения audioStream
    // Используется для правильной обработки video-only файлов
    int has_audio;  // 1 = есть аудио, 0 = video-only
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1)
    // Единственное место истины для AudioState - хранится ТОЛЬКО в native (C/C++)
    // Flutter никогда не вычисляет AudioState сам
    // Примечание: AudioStateEnum - это enum для audio_state в PlayerContext
    // struct AudioState (определён в audio_renderer.h) - это структура для audio renderer
    AudioStateEnum audio_state;  // Текущее состояние аудио (enum)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - AVSyncGate
    // Один объект. Одна правда. НЕ знает FSM. НЕ знает UI. НЕ знает Flutter.
    // Только master clock и его валидность.
    AVSyncGate avsync_gate;  // AVSYNC gate для контроля master clock (полное определение в avsync_gate.h)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - AvSyncState
    // Единый источник правды для AVSYNC состояния
    // Гарантирует устойчивость и recovery из рассинхрона
    AvSyncState avsync;  // AVSYNC state для hardening
    
    // Audio and Video states
    AudioState *audio;  // AudioState struct (legacy, для обратной совместимости)
    VideoState *video;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - AudioClock (PTS-based)
    // 🔒 AUDIO CLOCK = PTS, а не AudioTrack
    // AudioTrack используется ТОЛЬКО для вывода звука, не для clock
    // AudioClock теперь находится в AudioState, не в PlayerContext
    
    // Player state (для seek)
    PlayerState state;
    
    // State flags
    int abort;
    int paused;
    int rendering;  // 🔴 ЗАДАЧА 4: Флаг, что render loop запущен
    int shutting_down;  // 🔴 ШАГ 5: Флаг завершения работы (для корректного shutdown)
    int pending_play;  // 🔒 Флаг отложенного play (после attach surface)
    int prepared_emitted;  // 🔒 ШАГ I: Флаг, что prepared event уже отправлен (1 раз)
    int renderer_ready;  // 🔒 FIX Z35: Флаг готовности renderer (EGLSurface + render loop)
    int surface_attached;  // 🔒 FIX: Флаг, что SurfaceTexture прикреплён
    int decode_started;  // 🔒 FIX: Флаг, что decode/demux thread запущен
    int play_requested;  // 🔒 DIFF 2: Флаг, что play() был вызван (decode стартует ТОЛЬКО после play)
    int avsync_gate_open;  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-GATE - clocks и decode стартуют ТОЛЬКО после eglMakeCurrent успешно выполнен
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUTO-NEXT - EOF detection
    // Флаг, что достигнут конец файла (для защиты watchdog от ложных срабатываний)
    int eof_reached;  // 1 = EOF достигнут, watchdog должен быть отключён
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - SeekState (единый источник правды)
    // Гарантирует:
    // - НИ ОДИН thread не может блокироваться на другом
    // - Seek работает идемпотентно (хоть 10 раз подряд)
    // - AVI / FLV точно перематываются
    // - AVSYNC не ломается
    SeekState seek;  // Seek state для SEEK + AVSYNC PATCH
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.1: Seek Serial (основа всего)
    // Seek = смена эпохи. Serial - это "эпоха" для фильтрации старых кадров
    atomic_int seek_serial;  // Атомарный счётчик эпох seek (инкрементируется при каждом seek)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.1: PlaybackMode
    // Определяет режим воспроизведения для background/foreground
    PlaybackMode playback_mode;  // MODE_AV или MODE_AUDIO_ONLY
    
    // 🔥 PATCH 4: Токен плеера (playerToken) - идентифицирует сессию плеера
    // Используется для фильтрации устаревших событий от старых плееров
    int player_token;  // Токен плеера (устанавливается при prepare)
    
    // Legacy поля (для обратной совместимости)
    int seek_in_progress;  // DEPRECATED: используйте seek.in_progress
    int waiting_first_frame_after_seek;  // Флаг, что ждём первый кадр после seek
    double seek_target_pts;  // Целевой PTS для seek (для проверки первого кадра >= target)
    int64_t last_position_before_seek_ms;  // 🔥 КРИТИЧЕСКИЙ FIX: Последняя валидная позиция ДО seek (для блокировки position updates)
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.7: Scrub Spam Protection
    // Pending seek сохраняется, если seek уже выполняется
    // Выполняется после firstFrameAfterSeek
    double pending_seek_seconds;  // Pending seek target в секундах
    bool pending_seek_exact;      // Pending seek exact flag
    bool has_pending_seek;        // Флаг, что есть pending seek
    int64_t master_clock_ms;  // 🔥 КРИТИЧЕСКИЙ FIX: Master clock (video PTS) в миллисекундах - обновляется ТОЛЬКО после eglSwapBuffers
    int64_t last_render_ts_ms;  // 🔥 КРИТИЧЕСКИЙ FIX: RENDER_STALL_ASSERT - timestamp последнего успешного eglSwapBuffers (monotonic time)
    
    // Threads
    pthread_t demuxThread;
    pthread_t renderThread;  // 🔴 ЗАДАЧА 4: Render loop thread
    pthread_t avsyncWatchdogThread;  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF - watchdog thread для clock stall
    pthread_t seekWatchdogThread;  // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - watchdog thread для seek deadlock
    
    // JNI callbacks
    JavaVM *jvm;
    jobject jniCallback;
    jmethodID onReadyMethod;
    jmethodID onPositionMethod;
    jmethodID onEndedMethod;
    jmethodID onErrorMethod;

    // 🔴 ЗАДАЧА 6: Subtitles
    SubtitleManager subtitles;
    int subtitles_enabled;  // 1 = enabled, 0 = disabled

    // 🔴 ЗАДАЧА 5: Error handling
    int error;  // PlayerError (int для совместимости)
    int error_reported;  // Флаг, что ошибка была отправлена в Flutter
    pthread_mutex_t error_mutex;  // Mutex для error
} PlayerContext;

/// Открыть медиафайл
///
/// @param ctx Контекст плеера
/// @param path Путь к файлу
/// @return 0 при успехе, <0 при ошибке
int open_media(PlayerContext *ctx, const char *path);

/// Закрыть медиафайл и освободить ресурсы
///
/// @param ctx Контекст плеера
void close_media(PlayerContext *ctx);

/// Начать воспроизведение
///
/// @param ctx Контекст плеера
/// @return 0 при успехе, <0 при ошибке
int play(PlayerContext *ctx);

/// Приостановить воспроизведение
///
/// @param ctx Контекст плеера
void player_pause(PlayerContext *ctx);

/// Выполнить seek (Шаг 38.3)
///
/// ⚠️ UI НИЧЕГО не делает напрямую - только устанавливает флаг
/// Seek выполняется в demux thread
///
/// @param ctx Контекст плеера
/// @param seconds Целевая позиция в секундах
/// @param exact true = точный seek (decode & drop), false = fast seek (keyframe only)
/// @return 0 при успехе, <0 при ошибке
int player_seek(PlayerContext *ctx, double seconds, bool exact);

/// Выполнить fast seek (Phase 1, Шаг 38.4)
///
/// Прыгает на ближайший keyframe ≤ target
///
/// @param ctx Контекст плеера
/// @return 0 при успехе, <0 при ошибке
int perform_fast_seek(PlayerContext *ctx);

/// Выполнить exact seek (Phase 2, Шаг 38.6)
///
/// Декодирует и дропает кадры до target_pts
/// Вызывается из decode threads после fast seek
///
/// @param ctx Контекст плеера
/// @return 0 при успехе, <0 при ошибке
int perform_exact_seek(PlayerContext *ctx);

/// Установить режим повтора
///
/// @param ctx Контекст плеера
/// @param mode 0=off, 1=one, 2=all
void set_repeat_mode(PlayerContext *ctx, int mode);

/// Обработать EOF (вызывается из playback loop)
///
/// @param ctx Контекст плеера
void handle_eof(PlayerContext *ctx);

/// Уведомить Flutter о событии
///
/// @param ctx Контекст плеера
/// @param event Событие ("completed", "repeat_one", "next")
void notify_flutter_event(PlayerContext *ctx, const char *event);

/// Получить текущую позицию
///
/// @param ctx Контекст плеера
/// @return Позиция в миллисекундах
int64_t get_position(PlayerContext *ctx);

/// Получить длительность
///
/// @param ctx Контекст плеера
/// @return Длительность в миллисекундах
int64_t get_duration(PlayerContext *ctx);

/// Поток demux (главный поток для seek и EOF)
///
/// Читает пакеты из файла и распределяет их по очередям
/// Выполняет seek при запросе
/// Обрабатывает EOF
/// 🔒 FIX: Экспортируется для использования в native_player_jni.c
/// @param arg Указатель на PlayerContext
/// @return NULL
void *demux_thread(void *arg);

/// Проверить, инициализирован ли плеер
///
/// @param ctx Контекст плеера
/// @return true если инициализирован
bool is_initialized(PlayerContext *ctx);

/// Проверить, воспроизводится ли видео
///
/// @param ctx Контекст плеера
/// @return true если воспроизводится
bool is_playing(PlayerContext *ctx);

/// Инициализировать PlayerState
///
/// @param state Состояние для инициализации
void player_state_init(PlayerState *state);

/// Установить скорость воспроизведения (Шаг 39.7)
///
/// @param ctx Контекст плеера
/// @param speed Скорость (0.5 .. 3.0)
/// @return 0 при успехе, <0 при ошибке
int player_set_speed(PlayerContext *ctx, double speed);

// === 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING ===

/// Инициализировать AVSYNC state
///
/// @param ctx PlayerContext
/// @param has_audio Флаг наличия аудио
void avsync_init(PlayerContext *ctx, int has_audio);

/// Обновить AVSYNC state (master switch логика)
///
/// @param ctx PlayerContext
void avsync_update(PlayerContext *ctx);

/// Сбросить AVSYNC state (для seek)
///
/// @param ctx PlayerContext
void avsync_reset(PlayerContext *ctx);

/// Выполнить жёсткую ресинхронизацию (hard resync)
///
/// @param ctx PlayerContext
void avsync_hard_resync(PlayerContext *ctx);

#endif // FFMPEG_PLAYER_H

