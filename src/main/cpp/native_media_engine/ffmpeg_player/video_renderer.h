#ifndef VIDEO_RENDERER_H
#define VIDEO_RENDERER_H

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "packet_queue.h"
#include "frame_queue.h"
#include "audio_renderer.h"
#include "subtitle_manager.h"
#include "hw_accel.h"
#include "video_render_android.h"
#include "clock.h"
#include "video_sync.h"

// === 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY ===

/// Классификация кадров (обязательно)
/// Кадр может быть либо показан, либо отброшен. Кадр НИКОГДА не "ждёт лучшего времени".
typedef enum {
    FRAME_OK,              // pts валиден
    FRAME_NO_PTS,           // pts == NAN
    FRAME_PTS_BACKWARD,     // pts < last_pts
    FRAME_PTS_JUMP_FORWARD, // pts >> expected
} FramePtsClass;

// classify_frame_pts определена как static в video_render_gl.c, не экспортируем

// Legacy enum для обратной совместимости
typedef enum {
    FRAME_PTS_VALID,          // pts >= 0, монотонный
    FRAME_PTS_ZERO,           // pts == 0, но допустим (startup)
    FRAME_PTS_NAN,            // AV_NOPTS_VALUE / NaN
    FRAME_PTS_NON_MONOTONIC   // pts < last_pts
} FramePTSKind;

/// Legacy классификатор (deprecated, используйте classify_frame_pts)
FramePTSKind video_classify_pts(double raw_pts, double last_pts, int clock_valid);

/// Вычислить effective PTS с fallback
///
/// @param raw_pts Исходный PTS кадра
/// @param kind Классификация PTS
/// @param last_presented_pts PTS последнего отображённого кадра
/// @param estimated_frame_duration Оценка длительности кадра
/// @return Effective PTS для использования
double video_get_effective_pts(double raw_pts, FramePTSKind kind, double last_presented_pts, double estimated_frame_duration);

/// Оценить frame_duration из разницы PTS (self-correcting)
///
/// @param raw_pts Текущий PTS
/// @param last_pts Предыдущий PTS
/// @return Оценка длительности кадра (clamped)
double video_estimate_frame_duration(double raw_pts, double last_pts);

// Forward declarations
struct AVCodecContext;

/// Get format callback для MediaCodec (Шаг 24.4)
///
/// @param ctx Codec context
/// @param pix_fmts Список поддерживаемых pixel formats
/// @return Выбранный pixel format
enum AVPixelFormat mediacodec_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);

// Forward declaration для VideoState (полное определение ниже)
struct VideoState;

// === 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17 ===

/// Video clock (PTS-based, КАНОНИЧЕСКИЙ)
///
/// 🔒 VIDEO CLOCK = PTS последнего реально ОТОБРАЖЁННОГО кадра
/// ❌ НЕ frame_timer
/// ❌ НЕ vsync time
/// ❌ НЕ system time
/// ❌ НЕ decode PTS без render
///
/// Инварианты:
///   - ✅ Video clock = PTS последнего ПОКАЗАННОГО кадра
///   - ✅ Clock обновляется ТОЛЬКО после eglSwapBuffers
///   - ✅ Если last_present_ts > 700ms → stalled
typedef struct {
    double pts_sec;          // 🔥 PTS последнего ПОКАЗАННОГО кадра (seconds)
    int valid;               // Флаг валидности clock (1 = valid, 0 = invalid)
    double last_present_ts;  // monotonic time (sec) последнего обновления
} VideoClock;

/// Обработать MediaCodec frame (zero-copy, Шаг 24.5 + 25.4)
///
/// @param vs Video state
/// @param frame MediaCodec frame
/// @return 0 при успехе, <0 при ошибке
int video_handle_mediacodec_frame(struct VideoState *vs, AVFrame *frame);

/// Состояние видео декодера и рендерера
///
/// Управляет:
/// - Декодированием видео пакетов
/// - A/V синхронизацией (video sync к audio)
/// - Frame pacing (правильная частота кадров)
/// - Frame dropping (при отставании)
typedef struct {
    /// Codec context для видео
    AVCodecContext *codecCtx;
    
    /// 🔴 КРИТИЧНО: Сохраняем video_stream для доступа к time_base
    /// НЕ используем codecCtx->time_base - он часто = 0/0 или неправильный
    /// video_stream->time_base - это ЕДИНСТВЕННО правильный time_base для PTS
    AVStream *video_stream;
    
    /// Счётчик кадров для fallback PTS (если все источники PTS недоступны)
    int64_t frame_index;
    
    /// Очередь пакетов (из demux thread)
    PacketQueue *packetQueue;
    
    /// Очередь декодированных кадров (в video render thread)
    FrameQueue *frameQueue;
    
    /// PTS предыдущего кадра (для проверки монотонности)
    double last_pts;
    
    // === 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17 ===
    
    /// 🔒 Video clock (PTS-based, КАНОНИЧЕСКИЙ)
    ///
    /// 🔒 VIDEO CLOCK = PTS последнего реально ОТОБРАЖЁННОГО кадра
    /// ❌ НЕ frame_timer
    /// ❌ НЕ vsync time
    /// ❌ НЕ system time
    /// ❌ НЕ decode PTS без render
    /// ❌ НЕ last_duration
    /// ❌ НЕ render fps
    ///
    /// Инварианты:
    ///   - ✅ Video clock = PTS последнего ПОКАЗАННОГО кадра
    ///   - ✅ Clock обновляется ТОЛЬКО после eglSwapBuffers
    ///   - ✅ Если last_present_ts > 700ms → stalled
    VideoClock clock;  // 🔥 ЕДИНСТВЕННЫЙ VIDEO CLOCK
    
    /// Seek serial (для фильтрации старых эпох)
    int64_t serial;
    
    /// Флаг, что первый кадр отрисован
    int has_frame;
    
    // Legacy поля (deprecated, для обратной совместимости)
    double video_clock_pts;      // DEPRECATED: используйте clock.pts_sec
    double last_video_clock_pts; // DEPRECATED: используйте clock.pts_sec
    double frame_timer;          // ❌ УДАЛЕНО полностью (ШАГ 17.1)
    double clock_pts;            // DEPRECATED
    double clock_pts_time_sec;  // DEPRECATED
    int clock_valid;             // DEPRECATED: используйте clock.valid
    // Примечание: last_pts уже определён выше, не дублируем
    
    /// Предыдущий frame delay (deprecated, не используется для clock)
    double last_delay;
    
    /// Флаг прерывания потока
    int abort;
    
    /// Указатель на PlayerContext (для установки video_finished при EOF)
    void *player_ctx;
    
    /// Thread для декодирования
    pthread_t decodeThread;
    
    /// Флаг, что decode thread был запущен
    int decodeThread_started;
    
    /// Флаг, что decode thread был join'нут
    int decodeThread_joined;
    
    /// Thread для рендеринга
    pthread_t renderThread;
    
    /// Флаг, что render thread был запущен
    int renderThread_started;
    
    /// Флаг, что render thread был join'нут
    int renderThread_joined;
    
    /// Флаг, что video_threads_stop() уже был вызван (защита от повторного вызова)
    int threads_stopped;
    
    /// SwsContext для конвертации пикселей
    struct SwsContext *sws_ctx;
    
    /// Целевой формат пикселей (RGBA для Texture)
    enum AVPixelFormat target_format;
    
    /// Ширина целевого кадра
    int target_width;
    
    /// Высота целевого кадра
    int target_height;
    
    /// Менеджер субтитров
    SubtitleManager *subtitle_manager;
    
    /// Текущий video clock (обновляется при показе кадра) - DEPRECATED
    /// ❌ DEPRECATED: используйте clock (VideoClock)
    Clock video_clock;  // DEPRECATED: используйте clock.pts_sec
    
    /// Флаг паузы рендеринга
    int render_paused;
    
    /// Texture ID для Flutter (native)
    int64_t texture_id;
    
    /// Состояние аппаратного ускорения
    HWAccelState hw_accel;
    
    /// Видеорендер (VideoRenderAndroid для ANativeWindow)
    VideoRenderAndroid video_render;
    
    /// ✅ ШАГ 6.2: Флаг, что первый кадр был отправлен (для prepared event)
    /// Сбрасывается при seek для повторной отправки prepared
    int first_frame_sent;
    
    /// ✅ ШАГ 6.2: Флаги для предотвращения дубликатов событий
    int prepared_emitted;
    int playStarted_emitted;
    int completed_emitted;
    
    /// 🔒 FIX Z36: Буфер первого кадра (safety-net для AVI и коротких файлов)
    ///
    /// АРХИТЕКТУРНОЕ ОБОСНОВАНИЕ:
    /// ExoPlayer/BetterPlayer не теряют первый кадр, потому что декодирование и рендер
    /// жёстко связаны через Surface lifecycle (MediaCodec.configure(surface) → decode → render атомарно).
    ///
    /// FFmpeg-плеер имеет разделённые потоки (decode thread и render loop), поэтому:
    /// - decode может начаться до готовности render loop
    /// - первый кадр может быть декодирован до attach surface
    /// - для AVI/коротких файлов это критично (EOF до готовности renderer)
    ///
    /// Решение: буферизация первого кадра при декодировании и гарантированный рендер
    /// после готовности render loop. Это эквивалент implicit buffering в ExoPlayer.
    ///
    /// Первый кадр сохраняется при декодировании и рендерится гарантированно
    AVFrame *first_frame;  // Буферизованный первый кадр
    int first_frame_ready;  // Флаг, что первый кадр сохранён
    int first_frame_rendered;  // Флаг, что первый кадр отрисован
} VideoState;

/// Инициализировать видео декодер
///
/// @param vs Состояние видео
/// @param stream Видео стрим из AVFormatContext
/// @return 0 при успехе, <0 при ошибке
int video_decoder_init(VideoState *vs, AVStream *stream);

/// Инициализировать SwsContext для конвертации пикселей
///
/// @param vs Состояние видео
/// @param target_format Целевой формат (AV_PIX_FMT_RGBA)
/// @param target_width Ширина
/// @param target_height Высота
/// @return 0 при успехе, <0 при ошибке
int video_sws_init(VideoState *vs, enum AVPixelFormat target_format, int target_width, int target_height);

/// Запустить потоки декодирования и рендеринга видео
///
/// @param vs Состояние видео
/// @param as Состояние аудио (для A/V sync)
/// @return 0 при успехе, <0 при ошибке
int video_decode_thread_start(VideoState *vs, AudioState *as);

/// Остановить потоки декодирования и рендеринга видео
///
/// @param vs Состояние видео
void video_decode_thread_stop(VideoState *vs);

/// Остановить потоки декодирования и рендеринга видео (алиас)
///
/// @param vs Состояние видео
void video_threads_stop(VideoState *vs);

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
void video_clock_reset(VideoState *vs);

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
void video_clock_on_frame_render(VideoState *vs, AVFrame *frame);

/// Получить текущий video clock
///
/// @param vs Состояние видео
/// @return Текущий video clock в секундах, или NAN если невалиден
double video_get_clock(VideoState *vs);

/// Проверить ASSERT-ы для video clock (обязательные)
///
/// @param vs Состояние видео
/// @param ctx PlayerContext
void video_clock_assert(VideoState *vs, void *ctx);

/// Проверить video stall
///
/// @param c VideoClock
/// @return 1 если stalled, 0 если running
int video_clock_is_stalled(VideoClock *c);

/// Освободить ресурсы видео декодера
///
/// @param vs Состояние видео
void video_decoder_destroy(VideoState *vs);

/// Получить PTS кадра
///
/// @param frame Кадр
/// @param time_base Time base стрима
/// @return PTS в секундах, или NAN если не определено
double video_pts(AVFrame *frame, AVRational time_base);

/// Вычислить frame delay
///
/// @param vs Состояние видео
/// @param pts PTS текущего кадра
/// @return Delay в секундах
double compute_frame_delay(VideoState *vs, double pts);

/// Пороговые значения для A/V sync
#define AV_SYNC_THRESHOLD_MIN 0.04
#define AV_SYNC_THRESHOLD_MAX 0.1
#define AV_NOSYNC_THRESHOLD   10.0

// VideoSyncResult определён в video_sync.h (уже включён выше)

/// Получить PTS кадра (используя best_effort_timestamp)
///
/// @param frame Кадр
/// @param stream Видео стрим
/// @return PTS в секундах, или NAN если не определено
double video_get_pts(AVFrame *frame, AVStream *stream);

/// Рендерить кадр (вызывается из video render thread)
///
/// @param frame Кадр для рендеринга
/// @param vs Состояние видео
/// @return 0 при успехе, <0 при ошибке
int render_video_frame(AVFrame *frame, VideoState *vs);

/// Получить активный субтитр для текущего video_clock
///
/// @param vs Состояние видео
/// @return Указатель на активный субтитр, или NULL
const SubtitleItem *video_get_active_subtitle(VideoState *vs);

/// Установить менеджер субтитров
///
/// @param vs Состояние видео
/// @param sm Менеджер субтитров
void video_set_subtitle_manager(VideoState *vs, SubtitleManager *sm);

/// Инициализировать Texture для Flutter
///
/// @param vs Состояние видео
/// @param width Ширина
/// @param height Высота
/// @return Texture ID, или <0 при ошибке
int64_t video_texture_init(VideoState *vs, int width, int height);

/// Обновить Texture (вызывается на vsync)
///
/// @param vs Состояние видео
/// @param frame Кадр для рендеринга
/// @return 0 при успехе, <0 при ошибке
int video_texture_update(VideoState *vs, AVFrame *frame);

#endif // VIDEO_RENDERER_H

