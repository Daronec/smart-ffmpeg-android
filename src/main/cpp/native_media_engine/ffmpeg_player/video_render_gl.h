/// Рендерит FFmpeg decoded frames (software decode) в GPU через OpenGL ES
/// и отдаёт результат в Flutter Texture

#ifndef VIDEO_RENDER_GL_H
#define VIDEO_RENDER_GL_H

#include <jni.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include "libavutil/frame.h"
#include "libavutil/rational.h"
#include "clock.h"
#include "video_color_info.h"
#include <stdbool.h>

// Forward declarations

/// 🔴 ШАГ 3: Double buffering для Flutter ImageTexture
typedef struct FlutterImageBuffer {
    GLuint tex_id;      // GL texture ID для Flutter
    int width;
    int height;
    uint64_t frame_index; // Счётчик кадров для синхронизации
} FlutterImageBuffer;

// Forward declarations
// Полные определения должны быть включены в .c файлах ПЕРЕД video_render_gl.h
// AudioState и VideoState определены как typedef struct { ... } в соответствующих заголовках
// Frame и FrameQueue определены в frame_queue.h
struct Frame;
struct FrameQueue;
// AudioState и VideoState - анонимные структуры, поэтому используем просто типы
// Полные определения включены в .c файлах ПЕРЕД video_render_gl.h
struct AudioState;
struct VideoState;

/// Состояние рендерера (Шаг 35.1)
typedef enum {
    VR_STATE_UNINITIALIZED,
    VR_STATE_INITIALIZED,
    VR_STATE_READY,
    VR_STATE_RENDERING,
    VR_STATE_RELEASING
} VideoRenderState;

/// Тип цели рендеринга
typedef enum {
    RENDER_TARGET_NONE,           // Render target ещё не выбран (по умолчанию)
    RENDER_TARGET_SURFACE,        // Рендеринг в EGLSurface (ANativeWindow)
    RENDER_TARGET_IMAGE_TEXTURE   // Рендеринг в ImageTexture (FBO)
} RenderTarget;

/// OpenGL видеорендер для software decode
///
/// Конвертирует YUV420P кадры в RGB через GPU shaders
/// и рендерит в Flutter Texture через FBO (ШАГ 3)
typedef struct VideoRenderGL {
    /// EGL display
    EGLDisplay egl_display;
    
    /// EGL context
    EGLContext egl_context;
    
    /// EGL config
    EGLConfig egl_config;
    
    /// 🔴 ВРЕМЕННО: EGLSurface и ANativeWindow оставлены для компиляции
    /// Будет удалено после полной реализации FBO рендеринга
    EGLSurface egl_surface;
    void *native_window; // ANativeWindow*
    
    /// JavaVM для JNI
    JavaVM *jvm;
    
    /// Тип цели рендеринга
    RenderTarget render_target;
    
    /// 🔴 ШАГ 3: Flutter Texture ID
    int64_t flutter_texture_id;
    
    /// 🔴 ШАГ 3: OpenGL texture ID из SurfaceTexture (GL_TEXTURE_EXTERNAL_OES)
    /// Используется для рендеринга напрямую в texture SurfaceTexture
    GLuint surface_texture_gl_id;
    
    /// 🔴 ШАГ 3: Double buffering для Flutter ImageTexture
    FlutterImageBuffer flutter_buffers[2]; // Double buffer
    int flutter_write_index; // Индекс буфера для записи (render thread)
    int flutter_read_index;  // Индекс буфера для чтения (Flutter acquireLatestImage)
    pthread_mutex_t flutter_buffer_mutex; // Защита double buffer
    uint64_t flutter_frame_counter; // Счётчик кадров
    
    /// 🔴 ШАГ 3: FBO (Frame Buffer Object) для offscreen рендеринга
    /// Рендерим в FBO, затем копируем в flutter_buffers[write_index].tex_id
    GLuint fbo; // Frame Buffer Object
    GLuint fbo_texture; // RGBA texture для FBO (временная, для рендеринга)
    int fbo_width; // Ширина FBO (равна video_width)
    int fbo_height; // Высота FBO (равна video_height)
    
    /// 🔴 ЭТАЛОН: Флаг готовности ImageTexture (FBO создан и привязан)
    int image_texture_ready;
    
    /// OpenGL shader program
    GLuint shader_program;
    
    /// Vertex shader
    GLuint vertex_shader;
    
    /// Fragment shader
    GLuint fragment_shader;
    
    /// YUV textures (Frame 0 - current, Шаг 41.4)
    GLuint tex_y0;
    GLuint tex_u0;
    GLuint tex_v0;
    
    /// YUV textures (Frame 1 - next, для interpolation, Шаг 41.4)
    GLuint tex_y1;
    GLuint tex_u1;
    GLuint tex_v1;
    
    /// Legacy: старые текстуры (для обратной совместимости)
    GLuint tex_y;
    GLuint tex_u;
    GLuint tex_v;
    
    /// Vertex buffer
    GLuint vbo;
    
    /// Ширина видео
    int video_width;
    
    /// Высота видео
    int video_height;
    
    /// 🔴 ЭТАЛОН: Viewport размеры (размер Flutter view, не видео!)
    /// Используется в glViewport для правильного fullscreen
    int viewport_w;
    int viewport_h;
    
    /// 🔴 ШАГ 1: Размеры EGLSurface (размер SurfaceTexture, не видео!)
    /// Используется для вычисления aspect ratio scale
    int surface_w;
    int surface_h;
    
    /// Флаг инициализации
    bool initialized;
    
    /// Состояние рендерера (Шаг 35.1)
    VideoRenderState state;
    
    /// Mutex для синхронизации рендеринга (Шаг 35.1)
    pthread_mutex_t render_mutex;
    
    /// Time base для расчёта PTS
    AVRational time_base;
    
    /// Флаг паузы (Шаг 33.8)
    bool paused;
    
    /// Последний отрендеренный кадр (для pause, Шаг 33.8)
    AVFrame *last_frame;
    
    /// 🔥 PATCH 2: Флаг инициализации video clock с первого кадра
    int clock_initialized;
    
    /// 🔥 PATCH 3: Флаг первого отрендеренного кадра (никогда не дропать)
    int first_frame_rendered;
    
    /// 🔴 ШАГ 4: Флаг готовности плеера (decoder запущен, первый кадр получен)
    bool player_prepared;
    
    // === 🔴 ЭТАЛОН: Video-only playback clock (ШАГ I) ===
    
    /// 🔴 ЭТАЛОН: Video clock для video-only режима (PTS первого кадра)
    /// Инициализируется при первом кадре, обновляется после каждого рендера
    double video_clock;
    
    /// 🔴 ЭТАЛОН: Frame timer (реальное время первого кадра, секунды)
    /// Используется для расчёта elapsed time между кадрами
    double frame_timer;
    
    // === Interpolation (Шаг 41.2) ===
    
    /// Текущий кадр (указатель из FrameQueue, без ownership)
    struct Frame *current_frame;
    
    /// Следующий кадр (указатель из FrameQueue, без ownership)
    struct Frame *next_frame;
    
    /// Время показа текущего кадра (сек)
    double current_pts;
    
    /// Время показа следующего кадра (сек)
    double next_pts;
    
    /// Включена ли interpolation
    bool interpolation_enabled;
    
    /// Has next frame (для shader)
    bool has_next_frame;
    
    // === Adaptive Interpolation (Шаг 41.8) ===
    
    /// Статистика для adaptive interpolation (ШАГ 6)
    struct {
        /// PTS последнего кадра
        double last_pts;
        
        /// Средний интервал между кадрами
        double avg_frame_interval;
        
        /// Jitter (нестабильность PTS)
        double jitter;
        
        /// Счётчик кадров
        int frame_count;
        
        /// Счётчик дропнутых кадров
        int drop_count;
        
        /// Время последнего обновления
        double last_update_time;
        
        /// ШАГ 6.5: Cooldown для anti-flicker (гистерезис)
        int toggle_cooldown;
    } interp_stats;
    
    /// Режим interpolation (ШАГ 6)
    enum {
        INTERP_AUTO,      // Автоматическое включение/выключение
        INTERP_FORCE_ON,  // Всегда включено
        INTERP_FORCE_OFF  // Всегда выключено
    } interp_mode;
    
    // === ШАГ 8: Sub-pixel jitter compensation ===
    
    /// Состояние для temporal smoothing alpha
    struct {
        /// Последний сглаженный alpha
        float last_alpha;
        
        /// Флаг валидности (для первого кадра)
        bool alpha_valid;
    } interp_alpha;
    
    // === ШАГ 10.1: Persistent textures (ШАГ 11.1 - исправление) ===
    
    /// Флаг инициализации текстур (ШАГ 11.1 - перенесено из static)
    bool textures_initialized;
    
    /// Ширина текстур (ШАГ 11.1)
    int tex_w;
    
    /// Высота текстур (ШАГ 11.1)
    int tex_h;
    
    /// Флаг, что EGL context текущий (ШАГ 11.2 - оптимизация eglMakeCurrent)
    bool egl_current;
    
    // === ШАГ 4: Jitter buffer ===
    
    /// Флаг готовности jitter buffer (сбрасывается при seek)
    bool jitter_buffer_ready;
    
    // === ШАГ 11.1: Кешированные uniform locations ===
    
    /// Uniform locations (кешируются при init)
    struct {
        GLint tex_y0;
        GLint tex_u0;
        GLint tex_v0;
        GLint tex_y1;
        GLint tex_u1;
        GLint tex_v1;
        GLint uAlpha;
        GLint uHasNextFrame;
        GLint u_colorspace;
        GLint u_range;
        GLint u_is_hdr;
        GLint uTransform;      // Resize/rotation transform matrix
        GLint uRotation;       // Rotation angle (0/90/180/270)
        GLint uGestureScale;   // Gesture scale (pinch-to-zoom)
        GLint uGestureOffset;  // Gesture offset (pan) - vec2
        GLint uScaleX;         // 🔴 ЭТАЛОН: Scale X для aspect ratio fit modes
        GLint uScaleY;         // 🔴 ЭТАЛОН: Scale Y для aspect ratio fit modes
    } uniforms;
    
    // === Resize / Rotation ===
    
    /// 🔴 ЭТАЛОН: Режим масштабирования (как в VLC/ExoPlayer)
    enum {
        FIT_CONTAIN,   // contain - вписать целиком (чёрные полосы)
        FIT_COVER,     // cover - заполнить экран (обрезка)
        FIT_STRETCH,   // stretch - растянуть (искажает)
        FIT_ORIGINAL   // original - 1:1 пиксели
    } fit_mode;
    
    /// 🔴 ЭТАЛОН: Scale factors для vertex shader (устанавливаются через uniform)
    float scale_x;
    float scale_y;
    
    /// Layout параметры
    struct {
        float view_w;      // Ширина viewport (Flutter widget)
        float view_h;      // Высота viewport (Flutter widget)
        float video_w;     // Ширина видео
        float video_h;     // Высота видео
        int rotation;      // Поворот: 0 / 90 / 180 / 270
    } layout;
    
    // === Gestures (Scale / Pan / Zoom) ===
    
    /// Transform для жестов (pinch-to-zoom, pan)
    struct {
        float scale;       // 1.0 = normal, >1.0 = zoom in
        float offset_x;    // Смещение по X (NDC)
        float offset_y;    // Смещение по Y (NDC)
    } transform;
    
    // === Subtitle Safe-Area ===
    
    /// Safe-area для субтитров (из Flutter MediaQuery)
    struct {
        float safe_top;    // Safe area сверху
        float safe_bottom; // Safe area снизу
        float safe_left;   // Safe area слева
        float safe_right;  // Safe area справа
        bool is_hdr;       // Флаг HDR для контраста субтитров
    } subtitle_safe;
} VideoRenderGL;

/// Инициализировать OpenGL видеорендер (Шаг 35.2)
///
/// Инициализирует EGL и shaders, но НЕ создаёт EGLSurface
/// EGLSurface создаётся позже через video_render_gl_attach_window
///
/// @param vr Видеорендер
/// @param jvm JavaVM для JNI
/// @param width Ширина видео
/// @param height Высота видео
/// @param time_base Time base для расчёта PTS
/// @return 0 при успехе, <0 при ошибке
int video_render_gl_init(VideoRenderGL *vr,
                         JavaVM *jvm,
                         int width,
                         int height,
                         AVRational time_base);

/// 🔴 ВРЕМЕННО: Оставляем attach_window для компиляции
/// Будет заменено на register_image_texture после реализации FBO
int video_render_gl_attach_window(VideoRenderGL *vr, void *native_window);

/// 🔒 Проверяет, прикреплен ли window к renderer
int video_render_gl_has_window(VideoRenderGL *vr);

/// 🔴 ВРЕМЕННО: Оставляем detach_window для компиляции
int video_render_gl_detach_window(VideoRenderGL *vr);

/// 🔴 ШАГ 3: Регистрирует Flutter ImageTexture (БУДЕТ РЕАЛИЗОВАНО)
///
/// Инициализирует FBO и double buffering для Flutter ImageTexture
/// Вызывается из Kotlin после создания ImageTexture
///
/// @param vr Видеорендер
/// @param texture_id Flutter Texture ID
/// @param gl_texture_id OpenGL texture ID из SurfaceTexture (GL_TEXTURE_EXTERNAL_OES)
/// @param width Ширина видео
/// @param height Высота видео
/// @return 0 при успехе, <0 при ошибке
int video_render_gl_register_image_texture(VideoRenderGL *vr, int64_t texture_id, GLuint gl_texture_id, int width, int height);

/// 🔴 ШАГ 3: Отменяет регистрацию Flutter ImageTexture (БУДЕТ РЕАЛИЗОВАНО)
///
/// Освобождает FBO и double buffering
/// Render loop должен быть остановлен ДО вызова этой функции
///
/// @param vr Видеорендер
/// @return 0 при успехе, <0 при ошибке
int video_render_gl_unregister_image_texture(VideoRenderGL *vr);

/// Рендерить YUV кадр через OpenGL (Шаг 33.4, 35.4)
///
/// VSync-driven: вызывается только когда кадр готов к рендерингу
/// Zero-copy safety: frame НЕ сохраняется после вызова
///
/// @param vr Видеорендер
/// @param frame YUV420P кадр (не сохраняется после вызова)
/// @param master_clock Master clock (audio) для frame pacing (Шаг 33.3)
/// @return 0 при успехе, <0 при ошибке, 1 если кадр слишком рано (wait)
int video_render_gl_frame(VideoRenderGL *vr, AVFrame *frame, double master_clock);

/// Рендерить кадр(ы) с interpolation (Шаг 41.2, 41.3)
///
/// Рендерит один или два кадра с interpolation между ними
///
/// @param vr Видеорендер
/// @param frame0 Текущий кадр (обязателен)
/// @param frame1 Следующий кадр (может быть NULL)
/// @param alpha Interpolation factor (0.0 = frame0, 1.0 = frame1)
/// @return 0 при успехе, <0 при ошибке
int video_render_gl_draw(VideoRenderGL *vr, AVFrame *frame0, AVFrame *frame1, double alpha);

/// VSync-driven render loop (Шаг 33.6, 35.6, 41.9)
///
/// Извлекает кадры из frame_queue и рендерит их по VSync
/// Вызывается из render thread
///
/// @param vr Видеорендер
/// @param frame_queue Очередь кадров
/// @param audio_state Audio state для master clock
/// @param video_state Video state для subtitle_manager (Шаг 41.9)
/// @param abort Флаг прерывания
void video_render_gl_render_loop(VideoRenderGL *vr,
                                  struct FrameQueue *frame_queue,
                                  struct AudioState *audio_state,
                                  struct VideoState *video_state,
                                  int *abort);

/// Рендерить субтитры поверх видео (Шаг 28.7)
///
/// @param vr Видеорендер
/// @param subtitle_text Текст субтитра
/// @param audio_clock Текущий audio clock для синхронизации
void video_render_gl_subtitle(VideoRenderGL *vr, const char *subtitle_text, double audio_clock);

/// Очистить экран (при seek, Шаг 28.9)
///
/// @param vr Видеорендер
/// Очистить видеорендер (при seek или reset)
///
/// @param vr Видеорендер
/// @param seek_target Целевая позиция seek в секундах (0.0 = полный сброс)
void video_render_gl_clear(VideoRenderGL *vr, double seek_target);

/// Установить viewport размеры (размер Flutter view)
///
/// 🔴 ЭТАЛОН: Viewport должен быть равен размеру Flutter Texture widget
/// 🔴 УДАЛЕНО: Старая версия с 3 параметрами заменена на версию с 5 параметрами (ниже)

/// 🔴 ЭТАЛОН: Обновить aspect ratio scale factors
///
/// Вызывается автоматически при изменении viewport/video size/fit mode.
/// Вычисляет scale_x и scale_y на основе текущего fit_mode.
///
/// @param vr Видеорендер
void video_render_gl_update_aspect(VideoRenderGL *vr);

/// 🔴 ЭТАЛОН: Установить fit mode (contain/cover/stretch/original)
///
/// Режимы отображения видео (как в VLC/ExoPlayer):
/// - FIT_CONTAIN (0): вписать целиком (чёрные полосы)
/// - FIT_COVER (1): заполнить экран (обрезка)
/// - FIT_STRETCH (2): растянуть (искажает)
/// - FIT_ORIGINAL (3): 1:1 пиксели
///
/// @param vr Видеорендер
/// @param fit_mode Режим (0=contain, 1=cover, 2=stretch, 3=original)
void video_render_gl_set_fit_mode(VideoRenderGL *vr, int fit_mode);

/// Освободить ресурсы OpenGL видеорендера
///
/// @param vr Видеорендер
void video_render_gl_release(VideoRenderGL *vr);

/// Проверить, инициализирован ли рендер
///
/// @param vr Видеорендер
/// @return true если инициализирован
bool video_render_gl_is_initialized(VideoRenderGL *vr);

/// Установить паузу (Шаг 33.8)
///
/// @param vr Видеорендер
/// @param paused true = пауза, false = воспроизведение
void video_render_gl_set_paused(VideoRenderGL *vr, bool paused);

/// 🔴 ШАГ 4: Установить флаг готовности плеера
///
/// Вызывается из JNI когда decoder запущен и первый кадр готов.
/// Без этого флага render loop не будет рендерить кадры.
void video_render_gl_set_prepared(VideoRenderGL *vr, bool prepared);

/// 🔴 ШАГ 3: Уведомляет Flutter о новом кадре (после рендеринга в FBO)
///
/// Вызывается из render loop после того, как кадр отрендерен в flutter_buffers[write_index]
/// Делает swap double buffer и вызывает markTextureFrameAvailable()
///
/// @param vr Видеорендер
void video_render_gl_mark_frame_available(VideoRenderGL *vr);

/// 🔴 ШАГ 3: Flutter вызывает acquireLatestImage() - возвращаем GL texture
///
/// Вызывается из Flutter Engine когда нужен новый кадр
/// Возвращает GL texture ID из flutter_buffers[read_index]
///
/// @param vr Видеорендер
/// @param texture_id_out [out] GL texture ID для Flutter
/// @param width_out [out] Ширина текстуры
/// @param height_out [out] Высота текстуры
/// @return true если кадр доступен, false если нет
bool video_render_gl_acquire_latest_image(VideoRenderGL *vr, GLuint *texture_id_out, int *width_out, int *height_out);

/// Включить/выключить interpolation (Шаг 41.2)
///
/// @param vr Видеорендер
/// @param enabled true = включить interpolation
void video_render_gl_set_interpolation(VideoRenderGL *vr, bool enabled);

/// Установить режим interpolation (Шаг 41.8)
///
/// @param vr Видеорендер
/// @param mode 0=INTERP_AUTO, 1=INTERP_FORCE_ON, 2=INTERP_FORCE_OFF
void video_render_gl_set_interp_mode(VideoRenderGL *vr, int mode);

/// Установить viewport и параметры отображения (Resize / Rotation)
///
/// Вызывается при изменении размера Flutter widget или повороте видео
///
/// @param vr Видеорендер
/// @param view_w Ширина viewport (Flutter widget)
/// @param view_h Высота viewport (Flutter widget)
/// @param rotation Поворот: 0 / 90 / 180 / 270
/// @param scale_mode Режим масштабирования: 0=SCALE_FIT, 1=SCALE_FILL, 2=SCALE_STRETCH
void video_render_gl_set_viewport(VideoRenderGL *vr,
                                   float view_w,
                                   float view_h,
                                   int rotation,
                                   int scale_mode);

/// Установить transform для жестов (pinch-to-zoom, pan)
///
/// Применяет масштабирование и смещение к видео quad
/// Субтитры НЕ затрагиваются (рисуются отдельно в Flutter)
///
/// @param vr Видеорендер
/// @param scale_delta Изменение масштаба (1.0 = без изменений, >1.0 = zoom in)
/// @param dx Смещение по X (NDC, относительно текущего состояния)
/// @param dy Смещение по Y (NDC, относительно текущего состояния)
void video_render_gl_set_transform(VideoRenderGL *vr,
                                    float scale_delta,
                                    float dx,
                                    float dy);

/// Сбросить transform жестов (double-tap zoom reset)
///
/// @param vr Видеорендер
void video_render_gl_reset_transform(VideoRenderGL *vr);

/// Установить safe-area для субтитров
///
/// Вызывается из Flutter при изменении MediaQuery.padding
/// Используется для корректного позиционирования субтитров
///
/// @param vr Видеорендер
/// @param safe_top Safe area сверху (dp)
/// @param safe_bottom Safe area снизу (dp)
/// @param safe_left Safe area слева (dp)
/// @param safe_right Safe area справа (dp)
/// @param is_hdr Флаг HDR для контраста субтитров
void video_render_gl_set_subtitle_safe_area(VideoRenderGL *vr,
                                             float safe_top,
                                             float safe_bottom,
                                             float safe_left,
                                             float safe_right,
                                             bool is_hdr);

// 🔥 КРИТИЧЕСКИЙ FIX: VSYNC_DROP_DETECT - функции-геттеры для получения счетчиков
int64_t video_render_get_swap_count(void);
double video_render_get_first_swap_time(void);
int64_t video_render_get_last_swap_ts_ms(void);

// 🔥 КРИТИЧЕСКИЙ FIX: POWER_SAVE/APS_ASSERT - функция-геттер для получения FPS
int video_render_get_fps(void);

#endif // VIDEO_RENDER_GL_H

