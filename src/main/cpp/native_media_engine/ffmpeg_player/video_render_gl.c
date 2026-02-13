/// Шаг 28: OpenGL Video Renderer (YUV → RGB, Texture для Flutter)

// Включаем заголовки с полными определениями ПЕРЕД video_render_gl.h
#include "frame_queue.h"  // Для Frame и FrameQueue
#include "audio_renderer.h"  // Для AudioState
#include "video_renderer.h"  // Для VideoState
#include "ffmpeg_player.h"  // Для PlayerContext
#include "avsync_gate.h"  // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION
#include "ffmpeg_player_lifecycle.h"  // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - для seek_watchdog_stop
#include "subtitle_manager.h"  // Шаг 41.9: для subtitle_manager_get_active
#include "video_color_info.h"
#include "native_player_jni.h"  // JNI TextureRegistry glue и доступ к g_player_context
#include "libavutil/frame.h"  // для av_frame_get_best_effort_timestamp
#include "video_render_gl.h"  // Включаем последним, чтобы использовать полные определения
#include <android/log.h>
#include <android/native_window.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>  // для usleep
#include <time.h>    // для clock_gettime
#include "libavutil/time.h"  // для av_gettime_relative

#define LOG_TAG "VideoRenderGL"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// 🔥 КРИТИЧЕСКИЙ FIX: VSYNC_DROP_DETECT - глобальные счетчики для детектирования потери кадров
static int64_t g_swap_count = 0;
static double g_first_swap_time = 0.0;
static int64_t g_last_swap_ts_ms = 0;

// 🔥 КРИТИЧЕСКИЙ FIX: POWER_SAVE/APS_ASSERT - счетчики FPS для детектирования throttling
static int g_frame_counter = 0;
static int64_t g_fps_window_start_ms = 0;
static int g_last_fps = 0;

// Функция для получения текущего времени в миллисекундах (monotonic)
static inline int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/// Получить monotonic time в секундах
/// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.4: HOLD RULES
static inline double get_monotonic_time_sec(void) {
    return (double)now_ms() / 1000.0;  // миллисекунды → секунды
}

// 🔴 Compile-time флаг для debug логирования в render loop
// В release-сборке НЕ ДОЛЖНО БЫТЬ логов в render loop
//#define VIDEO_RENDER_DEBUG

// Forward declaration
static void compute_transform(VideoRenderGL *vr, float *out_mat4);

// Шаг 41.7: Пороги для frame drop & jitter protection
// 🔴 ШАГ 4+5: Обновлены пороги для стабилизации тайминга
#define MAX_VIDEO_LAG           0.100   // 100 ms → drop (слишком поздно)
#define MAX_VIDEO_LEAD          0.040   // 40 ms → wait (слишком рано)
#define AV_SYNC_THRESHOLD       0.100   // 🔴 ШАГ 5: Увеличен threshold для предотвращения burst-drop (не 0.04)
#define VIDEO_LATE_THRESHOLD   MAX_VIDEO_LAG   // Alias для обратной совместимости
#define VIDEO_EARLY_THRESHOLD  MAX_VIDEO_LEAD  // Alias для обратной совместимости
#define INTERP_MAX_GAP         (1.0 / 15.0)   // ~66.7 ms → no interpolation (1/15 fps)
#define INTERP_MIN_GAP         0.008   // 8 ms → no sense
#define JITTER_BUFFER_MIN      2       // Минимум кадров в буфере перед стартом рендеринга

// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.2: КАНОНИЧЕСКИЕ КОНСТАНТЫ
#define MAX_VIDEO_GAP_SEC       1.0     // защита от мусорных скачков (AVI/FLV)
#define AVSYNC_THRESHOLD        0.04    // 40ms (стандартный порог AVSYNC)
#define MAX_FRAME_HOLD_SEC      0.5     // защита от вечного hold (deadlock protection)
#define VIDEO_QUEUE_MAX         3       // Queue hard limit

// Legacy константы (deprecated, используйте новые)
#define DROP_THRESHOLD_SEC      0.120   // DEPRECATED: используйте AVSYNC_THRESHOLD
#define CLAMP_THRESHOLD_SEC     0.100   // DEPRECATED: используйте AVSYNC_THRESHOLD
#define SEEK_TOLERANCE_SEC      0.002   // 2ms → tolerance for seek target

// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - PATCH 2: Классификатор PTS
/// Классифицировать PTS кадра
///
/// @param pts PTS кадра в секундах
/// @param last_pts PTS предыдущего кадра
/// @param expected_delta Ожидаемая разница PTS (1/fps или avg_frame_duration)
/// @return Классификация кадра
static FramePtsClass classify_frame_pts(double pts, double last_pts, double expected_delta) {
    if (isnan(pts)) {
        return FRAME_NO_PTS;
    }
    
    if (!isnan(last_pts) && pts + 0.002 < last_pts) {
        return FRAME_PTS_BACKWARD;
    }
    
    if (!isnan(last_pts) && pts - last_pts > expected_delta * 10.0) {
        return FRAME_PTS_JUMP_FORWARD;
    }
    
    return FRAME_OK;
}

// Шаг 41.5: Получить PTS кадра в секундах
/// Вычисляет PTS кадра в секундах с использованием best_effort_timestamp
///
/// 🔴 КРИТИЧНО: Для FLV, B-frames и старых контейнеров frame->pts часто = 0 / AV_NOPTS_VALUE
/// ✅ Используем frame->best_effort_timestamp (поле структуры AVFrame) - гарантирует правильный PTS
static inline double frame_pts_sec(AVFrame *frame, AVRational time_base) {
    // 🔴 ЭТАЛОН: Используем frame->best_effort_timestamp (поле структуры AVFrame)
    // Это единственный правильный способ для FLV, B-frames, video-only файлов
    int64_t pts = frame->best_effort_timestamp;
    
    if (pts == AV_NOPTS_VALUE) {
        // Fallback на frame->pkt_dts если доступен
        pts = frame->pkt_dts;
    }
    
    if (pts == AV_NOPTS_VALUE) {
        // Последний fallback на frame->pts
        pts = frame->pts;
        if (pts == AV_NOPTS_VALUE) {
            return NAN;
        }
    }
    
    return (double)pts * av_q2d(time_base);
}

// Шаг 41.5: Расчёт alpha для interpolation
// 🔴 ИСПРАВЛЕНИЕ: Параметр переименован с audio_clock на master_time для ясности
// (может быть audio или video clock в зависимости от режима)
static float compute_interpolation_alpha(double master_time, double pts0, double pts1) {
    if (isnan(pts0) || isnan(pts1)) {
        return 0.0f;
    }
    
    double duration = pts1 - pts0;
    if (duration <= 0.0) {
        return 0.0f;
    }
    
    // 🔴 ШАГ 8: Правильный расчёт alpha на основе master_time
    // alpha = (master_time - frame0.pts) / (frame1.pts - frame0.pts)
    // alpha ∈ [0..1]: 0.0 = frame0, 1.0 = frame1
    double alpha = (master_time - pts0) / duration;
    
    // Clamp обязателен — VSync может прийти раньше/позже
    // ❌ НИКОГДА не выходить за [0..1] - это критично для стабильности
    if (alpha < 0.0) return 0.0f;
    if (alpha > 1.0) return 1.0f;
    
    return (float)alpha;
}

// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 9.4
/// Решение о дропе кадра (ЕДИНСТВЕННОЕ место принятия решения)
///
/// Кадр может быть либо показан, либо отброшен. Кадр НИКОГДА не "ждёт лучшего времени".
/// ❌ НЕ в decode
/// ❌ НЕ в demux
/// ❌ НЕ в queue push
/// ✅ ТОЛЬКО перед eglSwapBuffers
///
/// @param vr VideoRenderGL
/// @param vs VideoState
/// @param f0 Текущий кадр
/// @param frame_pts PTS кадра в секундах
/// @param frame_class Классификация PTS кадра
/// @param audio_clock Текущий audio clock (NAN если нет аудио)
/// @param master_time Master clock (audio или video)
/// @return 1 если кадр должен быть дропнут, 0 если должен быть отрендерен
/// 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.3: DROP RULES (ЖЁСТКИЕ)
///
/// Лучше выкинуть кадр, чем сломать clock
/// Видео — подчиняется AVSYNC, а не наоборот
///
/// @param vr VideoRenderGL
/// @param vs VideoState
/// @param f0 Frame
/// @param frame_pts PTS кадра в секундах
/// @param frame_class Классификация PTS кадра
/// @param audio_clock Audio clock в секундах
/// @param master_time Master clock в секундах
/// @return 1 если кадр должен быть отброшен, 0 если должен быть отрендерен
static int should_drop_frame(VideoRenderGL *vr, VideoState *vs, Frame *f0, 
                              double frame_pts, FramePtsClass frame_class,
                              double audio_clock, double master_time) {
    if (!vr || !f0) {
        return 0; // Не дропаем если нет данных
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.6: SEEK + FRAME POLICY
    // Первый кадр после seek: не применяем DROP по diff, не сравниваем с audio
    // Принимаем первый валидный PTS
    if (!vs || !vs->has_frame) {
        // Первый кадр — СВЯТОЙ (никогда не дропаем)
        // Это критично для seek, prepare, surface recreate
        return 0;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.3.1: NOPTS
    // 1️⃣ NOPTS → DROP
    if (isnan(frame_pts) || frame_pts < 0.0) {
        ALOGW("⚠️ FRAME DROP: NOPTS (drop, pts=%f)", frame_pts);
        return 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.3.2: Регрессия PTS
    // 2️⃣ Регрессия PTS → DROP
    if (!isnan(vs->last_pts) && frame_pts <= vs->last_pts) {
        ALOGW("⚠️ FRAME DROP: PTS regression (drop, pts=%.3f <= last=%.3f)", frame_pts, vs->last_pts);
        return 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.3.3: Слишком большой скачок вперёд
    // 3️⃣ Слишком большой скачок вперёд → DROP
    // Особенно важно для AVI
    if (!isnan(vs->last_pts) && frame_pts - vs->last_pts > MAX_VIDEO_GAP_SEC) {
        ALOGW("⚠️ FRAME DROP: PTS gap (drop, pts=%.3f > last=%.3f + %.1f)", 
              frame_pts, vs->last_pts, MAX_VIDEO_GAP_SEC);
        return 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.3.4: Видео убегает от аудио
    // 4️⃣ Видео убегает от аудио → DROP
    // diff = video_pts - audio_clock
    // if (diff > AVSYNC_THRESHOLD) → DROP
    if (!isnan(audio_clock) && !isnan(frame_pts)) {
        double diff = frame_pts - audio_clock;
        if (diff > AVSYNC_THRESHOLD) {
            ALOGW("⚠️ FRAME DROP: video ahead of audio (drop, diff=%.3f > threshold=%.3f)", 
                  diff, AVSYNC_THRESHOLD);
            return 1;
        }
    }
    
    // ❌ seek serial mismatch (используем seek_id из PlayerContext)
    if (vs && vs->player_ctx) {
        PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
        if (ctx->seek.in_progress && f0->serial != ctx->seek.seek_id) {
            ALOGW("⚠️ FRAME DROP: seek serial mismatch (drop, frame_serial=%d != seek_id=%ld)", 
                  f0->serial, (long)ctx->seek.seek_id);
            return 1;
        }
    }
    
    // ✅ Кадр прошёл все проверки → должен быть отрендерен
    return 0;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.3: get_master_clock()
/// Использует master из avsync для выбора ref_clock
/// ✅ ref_clock = (master == MASTER_AUDIO) ? audio.clock : video.clock
static inline double get_master_clock(AudioState *audio_state, VideoState *video_state) {
    if (!video_state || !video_state->player_ctx) {
        return 0.0;
    }
    
    PlayerContext *ctx = (PlayerContext *)video_state->player_ctx;
    
    // 🔥 ШАГ 20.3: Используем master из avsync для выбора ref_clock
    double ref_clock = (ctx->avsync.master == CLOCK_MASTER_AUDIO)
        ? ctx->avsync.audio_clock
        : ctx->avsync.video_clock;
    
    return ref_clock;
}

/// ШАГ 8: Temporal smoothing для alpha (sub-pixel jitter compensation)
static float smooth_alpha(VideoRenderGL *vr, float alpha_raw, double jitter) {
    if (!vr) {
        return alpha_raw;
    }
    
    // ШАГ 8.2: Первый кадр - инициализируем
    if (!vr->interp_alpha.alpha_valid) {
        vr->interp_alpha.last_alpha = alpha_raw;
        vr->interp_alpha.alpha_valid = true;
        return alpha_raw;
    }
    
    // ШАГ 8.5: Adaptive smoothing (сильнее при нестабильном FPS)
    float k = (jitter > 0.01) ? 0.85f : 0.7f; // ШАГ 8.5
    
    // ШАГ 8.2: 1-pole low-pass filter
    float alpha_smooth = vr->interp_alpha.last_alpha * k + alpha_raw * (1.0f - k);
    
    // Сохраняем для следующего кадра
    vr->interp_alpha.last_alpha = alpha_smooth;
    
    return alpha_smooth;
}

// 🔴 ТЕСТ: ЭТАЛОННЫЙ простой vertex shader (HiSilicon-safe)
// Раскомментируй для теста, закомментируй сложный shader ниже
/*
static const char *vertex_shader_source_test =
    "attribute vec4 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = aPosition;\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";
*/

// 🔴 ЭТАЛОННЫЙ Vertex Shader (HiSilicon-safe, с поддержкой aspect ratio fit modes)
// 🔴 ЭТАЛОН: Использует uniform uScaleX и uScaleY для управления aspect ratio
static const char *vertex_shader_source_etalon =
    "attribute vec4 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "uniform float uScaleX;\n"
    "uniform float uScaleY;\n"
    "varying vec2 vTexCoord;\n"
    "\n"
    "void main() {\n"
    "    gl_Position = vec4(\n"
    "        aPosition.x * uScaleX,\n"
    "        aPosition.y * uScaleY,\n"
    "        aPosition.z,\n"
    "        aPosition.w\n"
    "    );\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

// === Vertex Shader (Шаг 28.4 + Resize/Rotation + Gestures) ===
// 🔴 ВРЕМЕННО: Используем эталонный простой shader для теста
// Раскомментируй сложный shader после успешного теста
static const char *vertex_shader_source;
/*
static const char *vertex_shader_source =
    "attribute vec4 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "uniform mat4 uTransform;\n"
    "uniform int uRotation;\n"
    "uniform float uGestureScale;\n"
    "uniform vec2 uGestureOffset;\n"
    "\n"
    "vec2 rotate(vec2 v) {\n"
    "    if (uRotation == 90)\n"
    "        return vec2(-v.y, v.x);\n"
    "    if (uRotation == 180)\n"
    "        return vec2(-v.x, -v.y);\n"
    "    if (uRotation == 270)\n"
    "        return vec2(v.y, -v.x);\n"
    "    return v;\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    vec2 pos = rotate(aPosition.xy);\n"
    "    // Применяем resize/rotation transform\n"
    "    vec4 transformed = uTransform * vec4(pos, 0.0, 1.0);\n"
    "    // Применяем жесты (scale + pan)\n"
    "    transformed.xy = transformed.xy * uGestureScale + uGestureOffset;\n"
    "    gl_Position = transformed;\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";
*/

// 🔴 ТЕСТОВЫЙ Fragment Shader (для диагностики FBO/Flutter)
// Раскомментируй для теста красной заливки (должен появиться красный прямоугольник)
// Если красный виден → FBO + Flutter OK → проблема в YUV pipeline
// Если чёрный → проблема в FBO / draw / viewport
/*
static const char *fragment_shader_test_red =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
    "}\n";
*/

// 🔴 ЭТАЛОННЫЙ Fragment Shader (HiSilicon-safe, BT.601, без интерполяции)
// ✅ ИСПРАВЛЕНО: .g вместо .r, clamp, max для стабильности на Kirin/HiSilicon
// 🔴 КОНТРОЛЬНЫЙ ТЕСТ: Раскомментируй для проверки texcoord
// Ожидаемо: левый низ — чёрный, правый верх — жёлтый
// Если нет → texcoord ещё сломаны
/*
static const char *fragment_shader_source_etalon =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(vTexCoord.x, vTexCoord.y, 0.0, 1.0);\n"
    "}\n";
*/

static const char *fragment_shader_source_etalon =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "\n"
    "uniform sampler2D texY;\n"
    "uniform sampler2D texU;\n"
    "uniform sampler2D texV;\n"
    "\n"
    "void main() {\n"
    "    // 🔴 ФИКС №4: Для GL_LUMINANCE нужно брать .r, а не .g\n"
    "    float y = texture2D(texY, vTexCoord).r;\n"
    "    float u = texture2D(texU, vTexCoord).r - 0.5;\n"
    "    float v = texture2D(texV, vTexCoord).r - 0.5;\n"
    "    \n"
    "    // YUV420P → RGB (BT.601)\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.344136 * u - 0.714136 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    \n"
    "    gl_FragColor = vec4(clamp(r, 0.0, 1.0), clamp(g, 0.0, 1.0), clamp(b, 0.0, 1.0), 1.0);\n"
    "}\n";

// === Fragment Shader (YUV420P → RGB + Interpolation, Шаг 28.4, 40.4, 41.3) ===
// 🔴 ВРЕМЕННО: Используем эталонный простой shader для теста
// Раскомментируй сложный shader после успешного теста
static const char *fragment_shader_source;
/*
static const char *fragment_shader_source =
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "\n"
    "// Frame 0 (current)\n"
    "uniform sampler2D tex_y0;\n"
    "uniform sampler2D tex_u0;\n"
    "uniform sampler2D tex_v0;\n"
    "\n"
    "// Frame 1 (next, для interpolation)\n"
    "uniform sampler2D tex_y1;\n"
    "uniform sampler2D tex_u1;\n"
    "uniform sampler2D tex_v1;\n"
    "\n"
    "// Interpolation\n"
    "uniform float uAlpha;        // 0.0 = frame0, 1.0 = frame1\n"
    "uniform int uHasNextFrame;  // 0 = false, 1 = true (GLSL ES не поддерживает bool uniform)\n"
    "\n"
    "// Colorspace (Шаг 40)\n"
    "uniform int u_colorspace;   // 0=BT.601, 1=BT.709, 2=BT.2020\n"
    "uniform int u_range;        // 0=LIMITED, 1=FULL\n"
    "uniform int u_is_hdr;       // 0=SDR, 1=HDR\n"
    "\n"
    "// YUV → RGB матрицы (Шаг 40.3)\n"
    "mat3 yuv2rgb_601 = mat3(\n"
    "    1.1643,  0.0000,  1.5958,\n"
    "    1.1643, -0.39173, -0.81290,\n"
    "    1.1643,  2.017,  0.000\n"
    ");\n"
    "\n"
    "mat3 yuv2rgb_709 = mat3(\n"
    "    1.164,  0.000,  1.793,\n"
    "    1.164, -0.213, -0.533,\n"
    "    1.164,  2.112,  0.000\n"
    ");\n"
    "\n"
    "mat3 yuv2rgb_2020 = mat3(\n"
    "    1.1689,  0.0000,  1.6836,\n"
    "    1.1689, -0.1881, -0.6523,\n"
    "    1.1689,  2.1481,  0.0000\n"
    ");\n"
    "\n"
    "// YUV → RGB конвертация (Шаг 41.3)\n"
    "vec3 yuv_to_rgb(float y, float u, float v) {\n"
    "    // Шаг 40.5: Color range handling\n"
    "    if (u_range == 0) {  // LIMITED (MPEG range)\n"
    "        y = 1.1643 * (y - 0.0625);\n"
    "    } else {\n"
    "        y = 1.1643 * (y - 0.0625);\n"
    "    }\n"
    "    u = u - 0.5;\n"
    "    v = v - 0.5;\n"
    "    \n"
    "    vec3 yuv = vec3(y, u, v);\n"
    "    vec3 rgb;\n"
    "    \n"
    "    // Шаг 40.3: Применяем YUV→RGB матрицу\n"
    "    if (u_colorspace == 0) {  // BT.601\n"
    "        rgb = yuv2rgb_601 * yuv;\n"
    "    } else if (u_colorspace == 2) {  // BT.2020\n"
    "        rgb = yuv2rgb_2020 * yuv;\n"
    "    } else {  // BT.709 (default)\n"
    "        rgb = yuv2rgb_709 * yuv;\n"
    "    }\n"
    "    \n"
    "    return rgb;\n"
    "}\n"
    "\n"
    "// Tone mapping (Шаг 40.7 - Filmic, как VLC)\n"
    "vec3 filmicToneMap(vec3 x) {\n"
    "    x = max(vec3(0.0), x - vec3(0.004));\n"
    "    return (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);\n"
    "}\n"
    "\n"
    "// Gamma correction (Шаг 40.8)\n"
    "vec3 gammaCorrect(vec3 c) {\n"
    "    return pow(c, vec3(1.0 / 2.2));\n"
    "}\n"
    "\n"
    "// Sample YUV из текстур (Шаг 41.6 - правильная интерполяция в YUV)\n"
    "vec3 sampleYUV(sampler2D yTex, sampler2D uTex, sampler2D vTex, vec2 coord) {\n"
    "    float y = texture2D(yTex, coord).r;\n"
    "    float u = texture2D(uTex, coord).r - 0.5;\n"
    "    float v = texture2D(vTex, coord).r - 0.5;\n"
    "    return vec3(y, u, v);\n"
    "}\n"
    "\n"
    "// YUV → RGB конвертация (Шаг 41.6)\n"
    "vec3 yuvToRgb(vec3 yuv) {\n"
    "    float y = yuv.x;\n"
    "    float u = yuv.y;\n"
    "    float v = yuv.z;\n"
    "    \n"
    "    // Шаг 40.5: Color range handling\n"
    "    if (u_range == 0) {  // LIMITED (MPEG range)\n"
    "        y = 1.1643 * (y - 0.0625);\n"
    "    } else {\n"
    "        y = 1.1643 * (y - 0.0625);\n"
    "    }\n"
    "    \n"
    "    // Шаг 40.3: Применяем YUV→RGB матрицу\n"
    "    vec3 yuv_vec = vec3(y, u, v);\n"
    "    vec3 rgb;\n"
    "    \n"
    "    if (u_colorspace == 0) {  // BT.601\n"
    "        rgb = yuv2rgb_601 * yuv_vec;\n"
    "    } else if (u_colorspace == 2) {  // BT.2020\n"
    "        rgb = yuv2rgb_2020 * yuv_vec;\n"
    "    } else {  // BT.709 (default)\n"
    "        rgb = yuv2rgb_709 * yuv_vec;\n"
    "    }\n"
    "    \n"
    "    return rgb;\n"
    "}\n"
    "\n"
    "// Шаг 41.10: Edge-aware interpolation\n"
    "// Вычисляет luma (яркость) для edge detection\n"
    "float luma(vec3 rgb) {\n"
    "    return dot(rgb, vec3(0.299, 0.587, 0.114));\n"
    "}\n"
    "\n"
    "// ШАГ 9: Hash-based dithering (VLC / mpv style)\n"
    "// ДОЛЖНА быть объявлена ДО main() для GLSL ES совместимости\n"
    "float hash_dither(vec2 p) {\n"
    "    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    // Шаг 41.6: Читаем YUV frame0\n"
    "    vec3 yuv0 = sampleYUV(tex_y0, tex_u0, tex_v0, vTexCoord);\n"
    "    \n"
    "    // Шаг 41.6: Interpolation в YUV пространстве (не RGB!)\n"
    "    vec3 yuv = yuv0;\n"
    "    if (uHasNextFrame != 0) {\n"
    "        vec3 yuv1 = sampleYUV(tex_y1, tex_u1, tex_v1, vTexCoord);\n"
    "        \n"
    "        // Шаг 41.10: Edge-aware interpolation\n"
    "        // Конвертируем оба кадра в RGB для edge detection\n"
    "        vec3 rgb0 = yuvToRgb(yuv0);\n"
    "        vec3 rgb1 = yuvToRgb(yuv1);\n"
    "        \n"
    "        // Вычисляем разницу по luma (яркости)\n"
    "        float luma0 = luma(rgb0);\n"
    "        float luma1 = luma(rgb1);\n"
    "        float diff = abs(luma0 - luma1);\n"
    "        \n"
        "        // ШАГ 7: Motion-aware interpolation с soft threshold\n"
        "        // Используем smoothstep для плавного перехода между interpolation и nearest\n"
        "        float motion = smoothstep(0.05, 0.20, diff);\n"
        "        \n"
        "        // Интерполированное значение\n"
        "        vec3 interp = mix(yuv0, yuv1, uAlpha);\n"
        "        \n"
        "        // Ближайший кадр\n"
        "        vec3 nearest = (uAlpha < 0.5) ? yuv0 : yuv1;\n"
        "        \n"
        "        // Плавное смешивание: motion=0 → interpolation, motion=1 → nearest\n"
        "        yuv = mix(interp, nearest, motion);\n"
    "    }\n"
    "    \n"
    "    // Шаг 41.6: Конвертируем интерполированный YUV в RGB\n"
    "    vec3 rgb = yuvToRgb(yuv);\n"
    "    \n"
    "    // Шаг 40.7: Tone mapping для HDR\n"
    "    if (u_is_hdr == 1) {\n"
    "        rgb = filmicToneMap(rgb);\n"
    "    }\n"
    "    \n"
    "    // Шаг 40.8: Gamma correction (после tone mapping)\n"
    "    rgb = gammaCorrect(rgb);\n"
    "    \n"
    "    // ШАГ 9: Temporal-friendly dithering (убираем banding / постеризацию)\n"
    "    // Шум \"плывёт\" вместе с interpolation для отсутствия shimmer\n"
    "    // hash_dither() объявлена выше, перед main() для GLSL ES совместимости\n"
    "    float dither = (hash_dither(gl_FragCoord.xy + uAlpha * 13.0) - 0.5) / 255.0;\n"
    "    rgb += dither;\n"
    "    \n"
    "    gl_FragColor = vec4(rgb, 1.0);\n"
    "}\n";
*/

// === Quad vertices (fullscreen) ===
static const float quad_vertices[] = {
    // Position (x, y)    Texture (u, v)
    -1.0f, -1.0f,         0.0f, 1.0f,  // Bottom-left
     1.0f, -1.0f,         1.0f, 1.0f,  // Bottom-right
    -1.0f,  1.0f,         0.0f, 0.0f,  // Top-left
     1.0f,  1.0f,         1.0f, 0.0f,  // Top-right
};

// === Вспомогательные функции ===

/// Компилировать shader
static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
        ALOGE("Failed to create shader");
        return 0;
    }
    
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    
    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLint info_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 0) {
            char *info_log = (char *)malloc(info_len);
            glGetShaderInfoLog(shader, info_len, NULL, info_log);
            ALOGE("Shader compilation failed: %s", info_log);
            free(info_log);
        }
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

/// Создать shader program
static GLuint create_program(const char *vertex_source, const char *fragment_source) {
    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_source);
    if (!vertex_shader) {
        return 0;
    }
    
    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_source);
    if (!fragment_shader) {
        glDeleteShader(vertex_shader);
        return 0;
    }
    
    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return 0;
    }
    
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLint info_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &info_len);
        if (info_len > 0) {
            char *info_log = (char *)malloc(info_len);
            glGetProgramInfoLog(program, info_len, NULL, info_log);
            ALOGE("Program linking failed: %s", info_log);
            free(info_log);
        }
        glDeleteProgram(program);
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        return 0;
    }
    
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);
    
    return program;
}

/// Инициализировать EGL (Шаг 35.2 - без EGLSurface)
static int init_egl_context(VideoRenderGL *vr) {
    // Получаем EGL display
    vr->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (vr->egl_display == EGL_NO_DISPLAY) {
        ALOGE("Failed to get EGL display");
        return -1;
    }
    
    // Инициализируем EGL
    EGLint major, minor;
    if (!eglInitialize(vr->egl_display, &major, &minor)) {
        ALOGE("Failed to initialize EGL");
        return -1;
    }
    
    ALOGI("EGL initialized: %d.%d", major, minor);
    
    // Выбираем конфигурацию
    EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    
    EGLint num_configs;
    if (!eglChooseConfig(vr->egl_display, attribs, &vr->egl_config, 1, &num_configs)) {
        ALOGE("Failed to choose EGL config");
        return -1;
    }
    
    if (num_configs == 0) {
        ALOGE("No matching EGL config found");
        return -1;
    }
    
    // Создаём EGL context (Шаг 35.2 - без surface)
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    
    vr->egl_context = eglCreateContext(
        vr->egl_display,
        vr->egl_config,
        EGL_NO_CONTEXT,  // No shared context
        context_attribs
    );
    
    if (vr->egl_context == EGL_NO_CONTEXT) {
        ALOGE("Failed to create EGL context");
        return -1;
    }
    
    // НЕ создаём EGLSurface здесь (Шаг 35.2)
    // EGLSurface создаётся позже через video_render_gl_attach_window
    
    ALOGI("EGL context initialized (surface will be created later)");
    return 0;
}

/// Инициализировать OpenGL ресурсы
static int init_gl_resources(VideoRenderGL *vr) {
    // 🔴 Инициализируем shader sources (используем эталонные)
    vertex_shader_source = vertex_shader_source_etalon;
    fragment_shader_source = fragment_shader_source_etalon;
    
    // Создаём shader program (Шаг 28.4)
    vr->shader_program = create_program(vertex_shader_source, fragment_shader_source);
    if (!vr->shader_program) {
        ALOGE("Failed to create shader program");
        return -1;
    }
    
    // Шаг 41.4: Создаём YUV textures для frame0 и frame1
    glGenTextures(1, &vr->tex_y0);
    glGenTextures(1, &vr->tex_u0);
    glGenTextures(1, &vr->tex_v0);
    glGenTextures(1, &vr->tex_y1);
    glGenTextures(1, &vr->tex_u1);
    glGenTextures(1, &vr->tex_v1);
    
    // Legacy: для обратной совместимости
    vr->tex_y = vr->tex_y0;
    vr->tex_u = vr->tex_u0;
    vr->tex_v = vr->tex_v0;
    
    // Настраиваем Y texture (frame0)
    glBindTexture(GL_TEXTURE_2D, vr->tex_y0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Настраиваем U texture (frame0)
    glBindTexture(GL_TEXTURE_2D, vr->tex_u0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Настраиваем V texture (frame0)
    glBindTexture(GL_TEXTURE_2D, vr->tex_v0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Шаг 41.4: Настраиваем текстуры для frame1
    glBindTexture(GL_TEXTURE_2D, vr->tex_y1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, vr->tex_u1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glBindTexture(GL_TEXTURE_2D, vr->tex_v1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Создаём VBO для quad
    glGenBuffers(1, &vr->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vr->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_vertices), quad_vertices, GL_STATIC_DRAW);
    
    // ШАГ 11.1: Кешируем uniform locations
    vr->uniforms.tex_y0 = glGetUniformLocation(vr->shader_program, "tex_y0");
    vr->uniforms.tex_u0 = glGetUniformLocation(vr->shader_program, "tex_u0");
    vr->uniforms.tex_v0 = glGetUniformLocation(vr->shader_program, "tex_v0");
    vr->uniforms.tex_y1 = glGetUniformLocation(vr->shader_program, "tex_y1");
    vr->uniforms.tex_u1 = glGetUniformLocation(vr->shader_program, "tex_u1");
    vr->uniforms.tex_v1 = glGetUniformLocation(vr->shader_program, "tex_v1");
    vr->uniforms.uAlpha = glGetUniformLocation(vr->shader_program, "uAlpha");
    vr->uniforms.uHasNextFrame = glGetUniformLocation(vr->shader_program, "uHasNextFrame");
    vr->uniforms.u_colorspace = glGetUniformLocation(vr->shader_program, "u_colorspace");
    vr->uniforms.u_range = glGetUniformLocation(vr->shader_program, "u_range");
    vr->uniforms.u_is_hdr = glGetUniformLocation(vr->shader_program, "u_is_hdr");
    vr->uniforms.uTransform = glGetUniformLocation(vr->shader_program, "uTransform");
    vr->uniforms.uRotation = glGetUniformLocation(vr->shader_program, "uRotation");
    vr->uniforms.uGestureScale = glGetUniformLocation(vr->shader_program, "uGestureScale");
    vr->uniforms.uGestureOffset = glGetUniformLocation(vr->shader_program, "uGestureOffset");
    // 🔴 ЭТАЛОН: Uniform locations для aspect ratio fit modes
    vr->uniforms.uScaleX = glGetUniformLocation(vr->shader_program, "uScaleX");
    vr->uniforms.uScaleY = glGetUniformLocation(vr->shader_program, "uScaleY");
    
    // 🔴 ДИАГНОСТИКА: Проверяем uniform locations для fit modes
    if (vr->uniforms.uScaleX < 0 || vr->uniforms.uScaleY < 0) {
        ALOGW("⚠️ uScaleX or uScaleY uniform not found (shader may not support fit modes)");
    } else {
        ALOGI("✅ Fit mode uniforms: uScaleX=%d, uScaleY=%d", vr->uniforms.uScaleX, vr->uniforms.uScaleY);
    }
    
    // ШАГ 11.3: Отключаем ненужные функции OpenGL
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    
    ALOGI("OpenGL resources initialized");
    return 0;
}

int video_render_gl_init(VideoRenderGL *vr,
                         JavaVM *jvm,
                         int width,
                         int height,
                         AVRational time_base) {
    if (!vr || !jvm) {
        ALOGE("Invalid parameters for video_render_gl_init");
        return -1;
    }
    
    memset(vr, 0, sizeof(VideoRenderGL));
    
    vr->jvm = jvm;
    vr->egl_surface = EGL_NO_SURFACE; // 🔴 ВРЕМЕННО: для компиляции
    vr->native_window = NULL; // Будет установлен позже через attach_window
    // 🔴 ЭТАЛОН: render_target по умолчанию = NONE
    // Render target должен быть установлен явно (SURFACE или IMAGE_TEXTURE) перед стартом render loop
    vr->render_target = RENDER_TARGET_NONE;
    vr->video_width = width;
    vr->video_height = height;
    vr->viewport_w = 0; // 🔴 ЭТАЛОН: Инициализируем viewport как 0 (будет установлен из Flutter)
    vr->viewport_h = 0;
    vr->surface_w = 0; // 🔴 ШАГ 1: Инициализируем surface размеры (будут получены после создания EGLSurface)
    vr->surface_h = 0;
    vr->fit_mode = FIT_CONTAIN; // 🔴 ЭТАЛОН: По умолчанию contain (как VLC)
    vr->scale_x = 1.0f;
    vr->scale_y = 1.0f;
    
    // 🔴 ЭТАЛОН: Инициализируем aspect ratio (будет пересчитан при setViewport)
    // Пока используем scale = 1.0 (stretch) до установки viewport
    vr->time_base = time_base;
    
    // 🔴 ЭТАЛОН: Инициализируем aspect ratio (будет пересчитан при setViewport)
    // Пока используем scale = 1.0 (stretch) до установки viewport
    vr->state = VR_STATE_UNINITIALIZED;
    vr->paused = false;
    vr->clock_initialized = 0;  // 🔥 PATCH 2: Инициализация флага
    vr->first_frame_rendered = 0;  // 🔥 PATCH 3: Инициализация флага
    vr->player_prepared = false;  // 🔴 ШАГ 4: Плеер не готов до первого кадра
    // 🔴 ЭТАЛОН: Инициализация video-only clock (ШАГ I)
    vr->video_clock = 0.0;
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE FIX - PATCH 3: УДАЛЕНО frame_timer usage
    // frame_timer больше не используется для clock
    vr->last_frame = NULL;
    vr->jitter_buffer_ready = false; // 🔴 ШАГ 4: Jitter buffer сбрасывается при init
    pthread_mutex_init(&vr->render_mutex, NULL);
    
    // 🔴 ШАГ 3: Инициализация Flutter ImageTexture полей
    vr->flutter_texture_id = -1;
    vr->surface_texture_gl_id = 0; // OpenGL texture ID из SurfaceTexture
    vr->flutter_write_index = 0;
    vr->flutter_read_index = 1;
    vr->flutter_frame_counter = 0;
    pthread_mutex_init(&vr->flutter_buffer_mutex, NULL);
    vr->fbo = 0;
    vr->fbo_texture = 0;
    vr->fbo_width = 0;
    vr->fbo_height = 0;
    
    // ШАГ 8: Инициализация alpha smoothing
    vr->interp_alpha.last_alpha = 0.0f;
    vr->interp_alpha.alpha_valid = false;
    
    // Шаг 35.2: Инициализируем EGL context (без surface)
    if (init_egl_context(vr) < 0) {
        ALOGE("Failed to initialize EGL context");
        return -1;
    }
    
    // Создаём dummy surface для инициализации OpenGL ресурсов
    // (нужен для компиляции shaders)
    EGLint pbuffer_attribs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };
    
    EGLSurface dummy_surface = eglCreatePbufferSurface(
        vr->egl_display,
        vr->egl_config,
        pbuffer_attribs
    );
    
    if (dummy_surface == EGL_NO_SURFACE) {
        ALOGE("Failed to create dummy pbuffer surface");
        eglDestroyContext(vr->egl_display, vr->egl_context);
        return -1;
    }
    
    // Делаем context текущим для инициализации ресурсов
    if (!eglMakeCurrent(vr->egl_display, dummy_surface, dummy_surface, vr->egl_context)) {
        ALOGE("Failed to make EGL context current");
        eglDestroySurface(vr->egl_display, dummy_surface);
        eglDestroyContext(vr->egl_display, vr->egl_context);
        return -1;
    }
    
    // Инициализируем OpenGL ресурсы
    if (init_gl_resources(vr) < 0) {
        ALOGE("Failed to initialize OpenGL resources");
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(vr->egl_display, dummy_surface);
        eglDestroyContext(vr->egl_display, vr->egl_context);
        return -1;
    }
    
    // 🔴 КРИТИЧНО: ОБЯЗАТЕЛЬНО detach EGL context из JNI thread
    // ИНАЧЕ render loop в другом thread получит EGL_BAD_ACCESS
    // Context должен быть detached ДО старта render thread
    if (!eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        ALOGE("❌ Failed to detach EGL context from JNI thread after GL resources init");
        eglDestroySurface(vr->egl_display, dummy_surface);
        eglDestroyContext(vr->egl_display, vr->egl_context);
        return -1;
    }
    
    eglDestroySurface(vr->egl_display, dummy_surface);
    
    // 🔍 ДИАГНОСТИКА: Проверяем, что context действительно detached
    EGLContext current_after = eglGetCurrentContext();
    if (current_after != EGL_NO_CONTEXT) {
        ALOGW("⚠️ EGL context still current after detach: %p (expected EGL_NO_CONTEXT)", (void *)current_after);
        // Принудительно detach ещё раз
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else {
        ALOGI("✅ EGL context successfully detached from JNI thread after GL resources init");
    }
    
    vr->egl_current = false; // 🔴 КРИТИЧНО: Сбрасываем флаг, так как context detached
    
    // Инициализация layout (Resize/Rotation)
    vr->layout.view_w = 0.0f;
    vr->layout.view_h = 0.0f;
    vr->layout.video_w = (float)width;
    vr->layout.video_h = (float)height;
    vr->layout.rotation = 0;
    // 🔴 ЭТАЛОН: fit_mode инициализируется в video_render_gl_init
    
    // Инициализация transform (Gestures)
    vr->transform.scale = 1.0f;
    vr->transform.offset_x = 0.0f;
    vr->transform.offset_y = 0.0f;
    
    // Инициализация subtitle safe-area
    vr->subtitle_safe.safe_top = 0.0f;
    vr->subtitle_safe.safe_bottom = 0.0f;
    vr->subtitle_safe.safe_left = 0.0f;
    vr->subtitle_safe.safe_right = 0.0f;
    vr->subtitle_safe.is_hdr = false;
    
    vr->state = VR_STATE_INITIALIZED;
    vr->initialized = true;
    
    // Устанавливаем renderer для JNI callback
    native_player_set_renderer(vr);
    
    ALOGI("🔴 ШАГ 2: Video size: %dx%d", width, height);
    ALOGI("OpenGL video renderer initialized (%dx%d) - waiting for window", width, height);
    
    return 0;
}

/// Присоединить ANativeWindow (Шаг 35.3)
int video_render_gl_attach_window(VideoRenderGL *vr, void *native_window) {
    if (!vr || !native_window) {
        ALOGE("Invalid parameters for video_render_gl_attach_window");
        return -1;
    }
    
    // Разрешаем attach если состояние INITIALIZED или READY (можно переприкрепить)
    if (vr->state != VR_STATE_INITIALIZED && vr->state != VR_STATE_READY) {
        ALOGE("VideoRenderGL not initialized (state: %d)", vr->state);
        return -1;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    // Если уже есть surface - уничтожаем его (переприкрепление)
    if (vr->egl_surface != EGL_NO_SURFACE) {
        // Делаем context не текущим перед уничтожением surface
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(vr->egl_display, vr->egl_surface);
        vr->egl_surface = EGL_NO_SURFACE;
        vr->egl_current = false;
    }
    
    vr->native_window = native_window;
    
    // 🔴 КРИТИЧНО: Устанавливаем тип рендеринга на Surface
    vr->render_target = RENDER_TARGET_SURFACE;
    
    // Создаём EGLSurface из ANativeWindow (Шаг 35.3)
    ANativeWindow *window = (ANativeWindow *)native_window;
    vr->egl_surface = eglCreateWindowSurface(
        vr->egl_display,
        vr->egl_config,
        window,
        NULL
    );
    
    if (vr->egl_surface == EGL_NO_SURFACE) {
        ALOGE("Failed to create EGL surface");
        pthread_mutex_unlock(&vr->render_mutex);
        return -1;
    }
    
    // 🔴 ШАГ 1.1: ЖЁСТКАЯ ПРОВЕРКА BASELINE - красный экран для проверки пути OpenGL → EGLSurface → SurfaceTexture → Flutter
    // Если красный виден → путь работает на 100%
    eglMakeCurrent(vr->egl_display, vr->egl_surface, vr->egl_surface, vr->egl_context);
    
    // 🔴 ЭТАЛОН: Используем viewport из Flutter (размер экрана), а не размер surface
    // Если viewport не установлен - используем размер surface как fallback
    int viewport_w = vr->viewport_w > 0 ? vr->viewport_w : 0;
    int viewport_h = vr->viewport_h > 0 ? vr->viewport_h : 0;
    
    if (viewport_w == 0 || viewport_h == 0) {
        // Fallback: получаем размеры surface
        EGLint surface_w = 0, surface_h = 0;
        eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_WIDTH, &surface_w);
        eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_HEIGHT, &surface_h);
        viewport_w = surface_w;
        viewport_h = surface_h;
    }
    
    // ✅ ШАГ 2: Baseline test удалён - используем нормальный рендеринг
    // Красный экран был только для диагностики, теперь не нужен
    
    // Detach context после теста (render thread будет делать eglMakeCurrent позже)
    eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    vr->egl_current = false;
    
    // 🔴 ШАГ 3: Получаем размеры EGLSurface после создания
    // Это должно совпадать с размером SurfaceTexture
    EGLint surface_width = 0, surface_height = 0;
    if (!eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_WIDTH, &surface_width) ||
        !eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_HEIGHT, &surface_height)) {
        ALOGE("Failed to query EGL surface size");
        eglDestroySurface(vr->egl_display, vr->egl_surface);
        vr->egl_surface = EGL_NO_SURFACE;
        pthread_mutex_unlock(&vr->render_mutex);
        return -1;
    }
    
    // 🔴 ШАГ 3: Сохраняем размеры surface для вычисления aspect ratio
    vr->surface_w = (int)surface_width;
    vr->surface_h = (int)surface_height;
    ALOGI("🔴 ШАГ 3: Surface size: %dx%d", vr->surface_w, vr->surface_h);
    
    // 🔴 КРИТИЧНО: Проверяем, что размер EGLSurface НЕ 1x1
    // Если размер 1x1, значит SurfaceTexture.setDefaultBufferSize() не был вызван
    if (surface_width == 1 && surface_height == 1) {
        ALOGW("⚠️ EGLSurface size is 1x1 - SurfaceTexture.setDefaultBufferSize() was not called!");
        ALOGW("   Video will not be visible. Call setSurfaceSize() BEFORE nativeAttachWindow()");
        // НЕ возвращаем ошибку - продолжаем, но видео не будет видно
    } else {
        ALOGI("✅ EGLSurface size = %dx%d (matches SurfaceTexture)", surface_width, surface_height);
        
        // 🔴 ШАГ 4: Вычисляем aspect ratio scale после получения размеров surface
        if (vr->video_width > 0 && vr->video_height > 0) {
            video_render_gl_update_aspect(vr);
        }
    }
    
    // 🔴 КРИТИЧНО: НЕ делаем eglMakeCurrent() здесь (в JNI thread)
    // EGLContext должен быть сделан current ТОЛЬКО в render thread
    // Если сделать здесь, то render thread не сможет захватить контекст (EGL_BAD_ACCESS)
    // 
    // Правильная схема:
    // 1. attach_window: создаём EGLSurface (БЕЗ eglMakeCurrent)
    // 2. render_loop_start: запускаем render thread
    // 3. render_loop: делаем eglMakeCurrent() в render thread
    
    // Включаем VSync (Шаг 28.8, 33.7) - можно делать без current context
    eglSwapInterval(vr->egl_display, 1);
    
    // 🔴 КРИТИЧНО: НЕ устанавливаем egl_current = true здесь
    // Флаг будет установлен в render loop после успешного eglMakeCurrent()
    vr->egl_current = false;
    
    vr->state = VR_STATE_READY;
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("✅ ANativeWindow attached, EGL surface created (%dx%d)", surface_width, surface_height);
    
    return 0;
}

/// 🔴 ЗАДАЧА 4: Отсоединить ANativeWindow (detach window)
///
/// Освобождает EGLSurface, но НЕ освобождает EGLContext
/// Render loop должен быть остановлен ДО вызова этой функции
int video_render_gl_has_window(VideoRenderGL *vr) {
    if (!vr) {
        return 0;
    }
    // Проверяем, что native_window не NULL и render_target = SURFACE
    return (vr->native_window != NULL && vr->render_target == RENDER_TARGET_SURFACE) ? 1 : 0;
}

int video_render_gl_detach_window(VideoRenderGL *vr) {
    if (!vr) {
        ALOGE("video_render_gl_detach_window: vr is NULL");
        return -1;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    // 🔥 КРИТИЧНО: НЕ уничтожаем EGLSurface здесь (в JNI потоке)
    // EGLSurface ОБЯЗАН быть уничтожен в render thread (где он был создан)
    // Это делается в конце video_render_gl_render_loop() перед выходом из потока
    // 
    // ❌ НЕ ДЕЛАЕМ:
    // - eglMakeCurrent(NULL)
    // - eglDestroySurface
    //
    // ✅ ДЕЛАЕМ:
    // - Только очистка указателя на native_window
    // - EGL уничтожение - в render thread
    
    // Очищаем указатель на native_window (не требует EGL)
    vr->native_window = NULL;
    vr->egl_current = false;
    
    // Возвращаемся в состояние INITIALIZED (готов к новому attach)
    if (vr->state == VR_STATE_READY) {
        vr->state = VR_STATE_INITIALIZED;
    }
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("✅ video_render_gl_detach_window: Window detached (EGL will be destroyed in render thread)");
    
    return 0;
}

/// 🔴 ШАГ 3: Регистрирует Flutter ImageTexture (ЗАГЛУШКА - БУДЕТ РЕАЛИЗОВАНО)
///
/// Инициализирует FBO и double buffering для Flutter ImageTexture
int video_render_gl_register_image_texture(VideoRenderGL *vr, int64_t texture_id, GLuint gl_texture_id, int width, int height) {
    if (!vr) {
        ALOGE("video_render_gl_register_image_texture: vr is NULL");
        return -1;
    }
    
    ALOGI("🔄 Register ImageTexture: id=%lld glTextureId=%u size=%dx%d",
          (long long)texture_id, gl_texture_id, width, height);
    
    // 🔴 КРИТИЧНО: Устанавливаем тип рендеринга на ImageTexture
    vr->render_target = RENDER_TARGET_IMAGE_TEXTURE;
    
    vr->flutter_texture_id = texture_id;
    vr->fbo_width = width;
    vr->fbo_height = height;
    
    // Инициализация double buffer
    vr->flutter_write_index = 0;
    vr->flutter_read_index = 1;
    vr->flutter_frame_counter = 0;
    pthread_mutex_init(&vr->flutter_buffer_mutex, NULL);
    
    // Убеждаемся, что EGL context current (нужен для GL вызовов)
    EGLContext current_ctx = eglGetCurrentContext();
    if (current_ctx != vr->egl_context) {
        EGLSurface target_surface = EGL_NO_SURFACE; // ImageTexture не использует surface
        if (!eglMakeCurrent(vr->egl_display, target_surface, target_surface, vr->egl_context)) {
            ALOGE("❌ Cannot make EGL context current in register_image_texture");
            vr->image_texture_ready = 0;
            return -1;
        }
        vr->egl_current = true;
    } else {
        vr->egl_current = true;
    }
    
    // 🔥 КРИТИЧНО: Для Flutter ImageTexture мы создаём свою texture
    // Flutter ImageTexture не предоставляет GL texture ID напрямую (gl_texture_id всегда 0)
    // Мы создаём texture, привязываем её к FBO, и Flutter читает её через ImageTexture
    if (gl_texture_id == 0) {
        // Создаём texture для FBO (стандартный случай для ImageTexture)
        glGenTextures(1, &vr->fbo_texture);
        glBindTexture(GL_TEXTURE_2D, vr->fbo_texture);
        
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            NULL
        );
        
        glBindTexture(GL_TEXTURE_2D, 0);
        
        ALOGI("✅ Created FBO texture: tex=%u (Flutter ImageTexture will read this)", vr->fbo_texture);
    } else {
        // Если Flutter предоставил texture ID (редкий случай)
        vr->fbo_texture = gl_texture_id;
        ALOGI("✅ Using Flutter-provided texture: tex=%u", vr->fbo_texture);
    }
    
    // 🔴 КРИТИЧЕСКАЯ ПРОВЕРКА: texture должна быть создана
    if (vr->fbo_texture == 0) {
        ALOGE("❌ CRITICAL: Failed to create/get texture - fbo_texture=0");
        vr->image_texture_ready = 0;
        return -1;
    }
    
    // ============================================================
    // Создаём FBO и привязываем texture
    // ============================================================
    glGenFramebuffers(1, &vr->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, vr->fbo);
    
    // 🔥 КРИТИЧНО: Привязываем texture к FBO
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        vr->fbo_texture,  // Используем texture от Flutter ImageTexture
        0
    );
    
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ALOGE("❌ FBO incomplete: 0x%x (expected 0x%x=GL_FRAMEBUFFER_COMPLETE)", 
              status, GL_FRAMEBUFFER_COMPLETE);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &vr->fbo);
        vr->fbo = 0;
        vr->fbo_texture = 0;
        vr->image_texture_ready = 0;
        return -1;
    } else {
        ALOGI("✅ FBO complete: tex=%u fbo=%u size=%dx%d", 
              vr->fbo_texture, vr->fbo, width, height);
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // 🔴 ЭТАЛОН: Используем Flutter texture
    vr->surface_texture_gl_id = vr->fbo_texture;
    
    // 🔴 КРИТИЧНО: ОБЯЗАТЕЛЬНО detach EGL context из JNI thread после создания FBO
    // ИНАЧЕ render loop в другом thread получит EGL_BAD_ACCESS
    if (!eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT)) {
        ALOGE("❌ Failed to detach EGL context from JNI thread after FBO creation");
        vr->image_texture_ready = 0;
        return -1;
    }
    
    // 🔍 ДИАГНОСТИКА: Проверяем, что context действительно detached
    EGLContext current_after = eglGetCurrentContext();
    if (current_after != EGL_NO_CONTEXT) {
        ALOGW("⚠️ EGL context still current after detach in register_image_texture: %p (expected EGL_NO_CONTEXT)", 
              (void *)current_after);
        // Принудительно detach ещё раз
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    } else {
        ALOGI("✅ EGL context successfully detached from JNI thread after FBO creation");
    }
    
    vr->egl_current = false; // 🔴 КРИТИЧНО: Сбрасываем флаг, так как context detached
    
    // 🔴 КРИТИЧНО: Устанавливаем флаг готовности
    vr->image_texture_ready = 1;
    
    ALOGI("✅ ImageTexture registered: flutterId=%lld, glTex=%u (Flutter), ready=%d", 
          (long long)texture_id, vr->fbo_texture, vr->image_texture_ready);
    return 0;
}

/// 🔴 ШАГ 3: Отменяет регистрацию Flutter ImageTexture (ЗАГЛУШКА - БУДЕТ РЕАЛИЗОВАНО)
int video_render_gl_unregister_image_texture(VideoRenderGL *vr) {
    if (!vr) {
        ALOGE("video_render_gl_unregister_image_texture: vr is NULL");
        return -1;
    }
    
    ALOGI("🔄 video_render_gl_unregister_image_texture: Unregistering ImageTexture");
    
    pthread_mutex_lock(&vr->render_mutex);
    
    // 🔴 КРИТИЧНО: Освобождаем FBO и texture
    if (vr->fbo != 0) {
        // Убеждаемся, что EGL context current (нужен для GL вызовов)
        if (!vr->egl_current) {
            EGLSurface target_surface = (vr->render_target == RENDER_TARGET_IMAGE_TEXTURE) 
                ? EGL_NO_SURFACE 
                : vr->egl_surface;
            eglMakeCurrent(vr->egl_display, target_surface, target_surface, vr->egl_context);
            vr->egl_current = true;
        }
        
        // Отвязываем FBO
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        // Освобождаем FBO (texture принадлежит Flutter ImageTexture, не удаляем)
        glDeleteFramebuffers(1, &vr->fbo);
        // 🔴 КРИТИЧНО: НЕ удаляем fbo_texture - она принадлежит Flutter ImageTexture
        vr->fbo = 0;
        vr->fbo_texture = 0;
        
        // 🔴 КРИТИЧНО: Detach EGL context после GL вызовов
        // unregister может вызываться из JNI thread, поэтому нужно detach
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        vr->egl_current = false;
        
        ALOGI("✅ FBO released: fbo deleted, EGL context detached");
    }
    
    vr->flutter_texture_id = -1;
    vr->fbo_width = 0;
    vr->fbo_height = 0;
    vr->flutter_write_index = 0;
    vr->flutter_read_index = 1;
    vr->flutter_frame_counter = 0;
    vr->image_texture_ready = 0; // 🔴 КРИТИЧНО: Сбрасываем флаг готовности
    
    pthread_mutex_unlock(&vr->render_mutex);
    pthread_mutex_destroy(&vr->flutter_buffer_mutex);
    
    ALOGI("✅ video_render_gl_unregister_image_texture: ImageTexture unregistered");
    return 0;
}

/// Загрузить один AVFrame в YUV текстуры (ШАГ 10.1 - persistent textures, ШАГ 11.1 - исправлено)
static void upload_yuv_frame(VideoRenderGL *vr, GLuint tex_y, GLuint tex_u, GLuint tex_v,
                              AVFrame *frame, int width, int height) {
    if (!vr) {
        return;
    }
    
    // ШАГ 11.1: Используем vr->textures_initialized вместо static
    // ШАГ 10.1: Инициализируем текстуры один раз (persistent textures)
    if (!vr->textures_initialized || vr->tex_w != width || vr->tex_h != height) {
        // Y plane - выделяем память один раз
        glBindTexture(GL_TEXTURE_2D, tex_y);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_LUMINANCE,
            width,
            height,
            0,
            GL_LUMINANCE,
            GL_UNSIGNED_BYTE,
            NULL  // ШАГ 10.1: Выделяем память без данных
        );
        
        // U plane (половинный размер)
        glBindTexture(GL_TEXTURE_2D, tex_u);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_LUMINANCE,
            width / 2,
            height / 2,
            0,
            GL_LUMINANCE,
            GL_UNSIGNED_BYTE,
            NULL  // ШАГ 10.1: Выделяем память без данных
        );
        
        // V plane (половинный размер)
        glBindTexture(GL_TEXTURE_2D, tex_v);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_LUMINANCE,
            width / 2,
            height / 2,
            0,
            GL_LUMINANCE,
            GL_UNSIGNED_BYTE,
            NULL  // ШАГ 10.1: Выделяем память без данных
        );
        
        vr->textures_initialized = true;
        vr->tex_w = width;
        vr->tex_h = height;
    }
    
    // 🔴 ЭТАЛОН: Используем GL_LUMINANCE для совместимости с HiSilicon/Kirin
    // GL_RED может не работать на старых устройствах
    // Для теста используем ТОЛЬКО GL_LUMINANCE
    GLenum format = GL_LUMINANCE;
    
    // ШАГ 11.1: Определяем формат для GLES3+ (закомментировано для теста)
    /*
    #ifdef GL_ES_VERSION_3_0
    GLenum format = GL_RED;
    #else
    GLenum format = GL_LUMINANCE;
    #endif
    */
    
    // 🔴 ФИКС №2: Загрузка YUV с учётом stride (КРИТИЧНО для GLES2)
    // GL_UNPACK_ROW_LENGTH недоступен в GLES2, поэтому копируем данные построчно
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    // Y plane
    glBindTexture(GL_TEXTURE_2D, tex_y);
    if (frame->linesize[0] != width) {
        // Stride отличается от width - копируем построчно
        uint8_t *temp_buffer = (uint8_t *)malloc(width * height);
        if (temp_buffer) {
            const uint8_t *src = frame->data[0];
            uint8_t *dst = temp_buffer;
            for (int y = 0; y < height; y++) {
                memcpy(dst, src, width);
                src += frame->linesize[0];
                dst += width;
            }
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, 0,
                width,
                height,
                format,
                GL_UNSIGNED_BYTE,
                temp_buffer
            );
            free(temp_buffer);
        } else {
            // Fallback: загружаем без учета stride (может быть "лесенка")
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, 0,
                width,
                height,
                format,
                GL_UNSIGNED_BYTE,
                frame->data[0]
            );
        }
    } else {
        // Stride равен width - загружаем напрямую
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0, 0,
            width,
            height,
            format,
            GL_UNSIGNED_BYTE,
            frame->data[0]
        );
    }
    
    // U plane (половинный размер)
    glBindTexture(GL_TEXTURE_2D, tex_u);
    int u_width = width / 2;
    int u_height = height / 2;
    if (frame->linesize[1] != u_width) {
        // Stride отличается от width - копируем построчно
        uint8_t *temp_buffer = (uint8_t *)malloc(u_width * u_height);
        if (temp_buffer) {
            const uint8_t *src = frame->data[1];
            uint8_t *dst = temp_buffer;
            for (int y = 0; y < u_height; y++) {
                memcpy(dst, src, u_width);
                src += frame->linesize[1];
                dst += u_width;
            }
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, 0,
                u_width,
                u_height,
                format,
                GL_UNSIGNED_BYTE,
                temp_buffer
            );
            free(temp_buffer);
        } else {
            // Fallback: загружаем без учета stride
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, 0,
                u_width,
                u_height,
                format,
                GL_UNSIGNED_BYTE,
                frame->data[1]
            );
        }
    } else {
        // Stride равен width - загружаем напрямую
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0, 0,
            u_width,
            u_height,
            format,
            GL_UNSIGNED_BYTE,
            frame->data[1]
        );
    }
    
    // V plane (половинный размер)
    glBindTexture(GL_TEXTURE_2D, tex_v);
    int v_width = width / 2;
    int v_height = height / 2;
    if (frame->linesize[2] != v_width) {
        // Stride отличается от width - копируем построчно
        uint8_t *temp_buffer = (uint8_t *)malloc(v_width * v_height);
        if (temp_buffer) {
            const uint8_t *src = frame->data[2];
            uint8_t *dst = temp_buffer;
            for (int y = 0; y < v_height; y++) {
                memcpy(dst, src, v_width);
                src += frame->linesize[2];
                dst += v_width;
            }
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, 0,
                v_width,
                v_height,
                format,
                GL_UNSIGNED_BYTE,
                temp_buffer
            );
            free(temp_buffer);
        } else {
            // Fallback: загружаем без учета stride
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, 0,
                v_width,
                v_height,
                format,
                GL_UNSIGNED_BYTE,
                frame->data[2]
            );
        }
    } else {
        // Stride равен width - загружаем напрямую
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0, 0,
            v_width,
            v_height,
            format,
            GL_UNSIGNED_BYTE,
            frame->data[2]
        );
    }
}

/// Загрузить YUV plane в texture (ШАГ 11.1 - DEPRECATED, используется только для fallback)
/// ⚠️ ВАЖНО: Эта функция конфликтует с persistent textures (ШАГ 10.1)
/// Используется ТОЛЬКО в video_render_frame (legacy), не в video_render_gl_draw
static void upload_yuv_plane(GLuint texture, int width, int height, const uint8_t *data, int stride) {
    glBindTexture(GL_TEXTURE_2D, texture);
    
    // 🔴 ЭТАЛОН: Используем GL_LUMINANCE для совместимости с HiSilicon/Kirin
    // GL_RED может не работать на старых устройствах
    // Для теста используем ТОЛЬКО GL_LUMINANCE
    GLenum internal_format = GL_LUMINANCE;
    GLenum format = GL_LUMINANCE;
    
    // ШАГ 11.1: Определяем формат для GLES3+ (закомментировано для теста)
    /*
    #ifdef GL_ES_VERSION_3_0
    GLenum internal_format = GL_RED;
    GLenum format = GL_RED;
    #else
    GLenum internal_format = GL_LUMINANCE;
    GLenum format = GL_LUMINANCE;
    #endif
    */
    
    // Если stride совпадает с width - используем glTexImage2D
    if (stride == width) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            internal_format,
            width,
            height,
            0,
            format,
            GL_UNSIGNED_BYTE,
            data
        );
    } else {
        // Если stride отличается - используем glTexSubImage2D построчно
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            internal_format,
            width,
            height,
            0,
            format,
            GL_UNSIGNED_BYTE,
            NULL
        );
        
        for (int y = 0; y < height; y++) {
            glTexSubImage2D(
                GL_TEXTURE_2D,
                0,
                0, y,
                width, 1,
                format,
                GL_UNSIGNED_BYTE,
                data + y * stride
            );
        }
    }
}

int video_render_gl_frame(VideoRenderGL *vr, AVFrame *frame, double master_clock) {
    if (!vr || !frame) {
        return -1;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    if (vr->state != VR_STATE_READY && vr->state != VR_STATE_RENDERING) {
        pthread_mutex_unlock(&vr->render_mutex);
        return -1;
    }
    
    // Шаг 33.8: Pause handling
    if (vr->paused) {
        // Сохраняем последний кадр (Шаг 33.8)
        if (vr->last_frame) {
            av_frame_free(&vr->last_frame);
        }
        vr->last_frame = av_frame_clone(frame);
        pthread_mutex_unlock(&vr->render_mutex);
        return 0; // Не рендерим, но сохраняем кадр
    }
    
    // Шаг 33.4: Frame pacing logic
    // 🔴 ЭТАЛОН: PTS calculation с правильным fallback chain
    // FFmpeg НЕ ГАРАНТИРУЕТ, что AVFrame.pts будет заполнен
    // ОЧЕНЬ ЧАСТО: frame->pts == AV_NOPTS_VALUE
    // ✅ ВСЕГДА использовать frame->best_effort_timestamp (поле структуры AVFrame)
    
    // 1. best effort timestamp (ОБЯЗАТЕЛЬНО - поле структуры AVFrame)
    int64_t pts_raw = frame->best_effort_timestamp;
    
    // 2. fallback chain (если best_effort недоступен)
    if (pts_raw == AV_NOPTS_VALUE) {
        pts_raw = frame->pkt_dts;
    }
    
    if (pts_raw == AV_NOPTS_VALUE) {
        pts_raw = frame->pts; // Последний fallback
    }
    
    // 3. convert to seconds
    // 🔴 КРИТИЧНО: Используем vr->time_base (который должен быть из video_stream->time_base)
    double video_pts = pts_raw == AV_NOPTS_VALUE
        ? NAN
        : (double)pts_raw * av_q2d(vr->time_base);
    
    // 4. защита от NaN / отрицательных
    if (!isfinite(video_pts) || video_pts < 0) {
        video_pts = 0.0;
    }
    
    if (!isnan(video_pts) && !isnan(master_clock)) {
        double delay = video_pts - master_clock;
        
        // Шаг 33.4: Если кадр слишком рано - ждём
        if (delay > 0.01) { // 10ms threshold
            // ❌ УБРАНО: Логирование "Frame too early" каждый кадр (забивает Logcat)
            // Это нормальное поведение - кадр ждёт синхронизации
            pthread_mutex_unlock(&vr->render_mutex);
            return 1; // Слишком рано, ждём
        }
        
        // Шаг 33.4: Если кадр сильно опоздал - дропаем
        if (delay < -0.1) { // 100ms threshold
            // Логируем как WARNING только аномалии (кадр слишком поздно)
            ALOGW("Frame too late: pts=%.3f master=%.3f delay=%.3f (drop)", 
                  video_pts, master_clock, delay);
            pthread_mutex_unlock(&vr->render_mutex);
            return -2; // Слишком поздно, дропаем
        }
    }
    
    // Проверяем формат (должен быть YUV420P)
    if (frame->format != AV_PIX_FMT_YUV420P) {
        ALOGE("Unsupported pixel format: %d (expected YUV420P)", frame->format);
        pthread_mutex_unlock(&vr->render_mutex);
        return -1;
    }
    
    // 🔴 ШАГ 3: В render loop контекст ВСЕГДА current
    // Проверка egl_current не нужна - контекст устанавливается один раз в начале render loop
    // Эта функция вызывается только из render loop, где контекст гарантированно current
    
    vr->state = VR_STATE_RENDERING;
    
    // 🔴 ЭТАЛОН: Используем viewport из Flutter (размер экрана), а не размер surface
    // Если viewport не установлен - используем размер surface как fallback
    int viewport_w = vr->viewport_w > 0 ? vr->viewport_w : 0;
    int viewport_h = vr->viewport_h > 0 ? vr->viewport_h : 0;
    
    if (viewport_w == 0 || viewport_h == 0) {
        // Fallback: получаем размеры surface
        EGLint surface_w = 0, surface_h = 0;
        eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_WIDTH, &surface_w);
        eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_HEIGHT, &surface_h);
        viewport_w = surface_w;
        viewport_h = surface_h;
    }
    
    // ✅ ШАГ 6.5: Проверяем EGL context перед GL вызовами
    EGLContext current_ctx = eglGetCurrentContext();
    if (current_ctx == EGL_NO_CONTEXT || current_ctx != vr->egl_context) {
        ALOGE("❌ EGL context not current in video_render_gl_frame: current=%p, expected=%p", 
              (void *)current_ctx, (void *)vr->egl_context);
        pthread_mutex_unlock(&vr->render_mutex);
        return -1; // ✅ КРИТИЧНО: Функция возвращает int, нужно вернуть значение ошибки
    }
    
    // 🔴 ЭТАЛОН: glViewport всегда на весь экран (aspect ratio управляется через uniform в vertex shader)
    if (viewport_w > 0 && viewport_h > 0) {
        glViewport(0, 0, viewport_w, viewport_h);
    } else {
        // Fallback на размеры видео (не должно происходить)
        ALOGW("⚠️ Cannot get viewport size, using video size as fallback");
        glViewport(0, 0, frame->width, frame->height);
    }
    
    // ШАГ 11.1: Используем upload_yuv_frame вместо upload_yuv_plane (persistent textures)
    upload_yuv_frame(vr, vr->tex_y, vr->tex_u, vr->tex_v, frame, frame->width, frame->height);
    
    // Используем shader program
    glUseProgram(vr->shader_program);
    
    // 🔴 ШАГ 6: Передаём scale в shader (делается при каждом resize/rotate, не каждый frame)
    if (vr->uniforms.uScaleX >= 0 && vr->uniforms.uScaleY >= 0) {
        glUniform1f(vr->uniforms.uScaleX, vr->scale_x);
        glUniform1f(vr->uniforms.uScaleY, vr->scale_y);
    } else {
        ALOGW("⚠️ ШАГ 6: uScaleX or uScaleY uniform not found (shader may not support aspect ratio)");
    }
    
    // 🔴 ЭТАЛОН: Проверяем uniform locations для простого shader (texY, texU, texV)
    // Это критично для правильной работы на HiSilicon/Kirin
    GLint texY_loc = glGetUniformLocation(vr->shader_program, "texY");
    GLint texU_loc = glGetUniformLocation(vr->shader_program, "texU");
    GLint texV_loc = glGetUniformLocation(vr->shader_program, "texV");
    
    // ✅ ШАГ 6.6: Убираем log-spam - логируем только в debug режиме
    #ifdef VIDEO_RENDER_DEBUG
    ALOGD("🔍 Legacy shader uniforms: texY=%d, texU=%d, texV=%d", texY_loc, texU_loc, texV_loc);
    #endif
    
    // Если простой shader (эталонный) - используем texY/texU/texV
    if (texY_loc >= 0 && texU_loc >= 0 && texV_loc >= 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vr->tex_y);
        glUniform1i(texY_loc, 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, vr->tex_u);
        glUniform1i(texU_loc, 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vr->tex_v);
        glUniform1i(texV_loc, 2);
    } else {
        // Fallback на кешированные uniform locations (сложный shader)
        ALOGI("⚠️ Using cached uniform locations (complex shader)");
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vr->tex_y);
        if (vr->uniforms.tex_y0 >= 0) glUniform1i(vr->uniforms.tex_y0, 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, vr->tex_u);
        if (vr->uniforms.tex_u0 >= 0) glUniform1i(vr->uniforms.tex_u0, 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vr->tex_v);
        if (vr->uniforms.tex_v0 >= 0) glUniform1i(vr->uniforms.tex_v0, 2);
    }
    
    // ШАГ 11.1: Используем кешированные colorspace uniforms
    VideoColorInfo color_info;
    video_color_info_from_frame(frame, &color_info);
    
    if (vr->uniforms.u_colorspace >= 0) {
        glUniform1i(vr->uniforms.u_colorspace, video_color_info_get_colorspace_index(&color_info));
    }
    if (vr->uniforms.u_range >= 0) {
        glUniform1i(vr->uniforms.u_range, video_color_info_get_range_index(&color_info));
    }
    if (vr->uniforms.u_is_hdr >= 0) {
        glUniform1i(vr->uniforms.u_is_hdr, video_color_info_is_hdr(&color_info) ? 1 : 0);
    }
    
    // Устанавливаем vertex attributes
    GLint pos_loc = glGetAttribLocation(vr->shader_program, "aPosition");
    GLint tex_loc = glGetAttribLocation(vr->shader_program, "aTexCoord");
    
    glBindBuffer(GL_ARRAY_BUFFER, vr->vbo);
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(tex_loc);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    
    // Очищаем экран
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // Рисуем quad
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // 🔴 ЭТАЛОН: Проверка GL ошибок после draw
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        ALOGE("❌ GL ERROR after glDrawArrays: 0x%x", err);
    }
    
    // 🔴 ШАГ 5: ЕДИНСТВЕННОЕ МЕСТО для eglSwapBuffers при рендере кадра
    // eglSwapBuffers = показ КАДРА, нет кадра → нет swap
    // eglSwapBuffers уведомляет Flutter автоматически через SurfaceTexture
    
    // 🔎 DIAGNOSTIC: Log frame info before swap
    double frame_pts_sec_val = NAN;
    if (frame && vr->time_base.num > 0 && vr->time_base.den > 0) {
        if (frame->pts != AV_NOPTS_VALUE) {
            frame_pts_sec_val = frame->pts * av_q2d(vr->time_base);
        } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            frame_pts_sec_val = frame->best_effort_timestamp * av_q2d(vr->time_base);
        }
    }
    ALOGI("🔁 GL SWAP: hasFrame=%d textureReady=%d frameSize=%dx%d pts=%.3f",
          frame != NULL ? 1 : 0,
          vr->textures_initialized ? 1 : 0,
          frame ? frame->width : 0,
          frame ? frame->height : 0,
          frame_pts_sec_val);
    
    eglSwapBuffers(vr->egl_display, vr->egl_surface);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.3
    // 🔥 ЕДИНСТВЕННОЕ место обновления video clock
    // ⚠️ НЕ в decode
    // ⚠️ НЕ в demux
    // ⚠️ НЕ в render loop tick
    // ⚠️ НЕ в vsync
    // Используем каноническую функцию video_clock_on_frame_render()
    // Примечание: VideoState получаем через g_player_context
    extern PlayerContext *g_player_context;
    if (g_player_context && g_player_context->video && frame) {
        extern void video_clock_on_frame_render(VideoState *vs, AVFrame *frame);
        video_clock_on_frame_render(g_player_context->video, frame);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 9.10: ASSERT для queue size
    // Примечание: frame_queue не доступен в этой функции, проверка выполняется в render loop
    #ifdef DEBUG
    // ASSERT для queue size выполняется в video_render_gl_render_loop
    #endif
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VSYNC_DROP_DETECT - обновляем счетчики ПОСЛЕ каждого успешного eglSwapBuffers
    int64_t current_ms = now_ms();
    if (g_swap_count == 0) {
        g_first_swap_time = current_ms / 1000.0;  // В секундах
    }
    g_swap_count++;
    g_last_swap_ts_ms = current_ms;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - эмитим firstFrameAfterSeek ПОСЛЕ swapBuffers
    // Это гарантирует, что кадр реально показан на экране, а не только декодирован
    extern PlayerContext *g_player_context;
    if (g_player_context) {
        PlayerContext *ctx = g_player_context;
        if (ctx->waiting_first_frame_after_seek) {
            // ✅ ПЕРВЫЙ КАДР ≥ target реально отрисован
            ctx->waiting_first_frame_after_seek = 0;
            
            // Эмитим firstFrameAfterSeek событие
            extern void native_player_emit_first_frame_after_seek_event(void);
            native_player_emit_first_frame_after_seek_event();
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.4: AFTER firstFrameAfterSeek
            // if (audio.playing && !audio.stalled) → master = MASTER_AUDIO
            avsync_gate_set_seek_in_progress(&ctx->avsync_gate, false);
            
            // 🔥 ШАГ 20.4: Проверяем, можно ли переключиться на AUDIO master
            if (ctx->has_audio && ctx->audio) {
                extern int audio_clock_is_stalled(AudioClock *c);
                bool audio_playing = ctx->audio_state == AUDIO_PLAYING;
                bool audio_stalled = audio_clock_is_stalled(&ctx->audio->clock);
                
                if (audio_playing && !audio_stalled) {
                    // 🔥 ШАГ 20.4: После firstFrameAfterSeek, если audio.playing && !audio.stalled → master = MASTER_AUDIO
                    ctx->avsync.master = CLOCK_MASTER_AUDIO;
                    ctx->avsync.audio_healthy = 1;
                    avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
                    avsync_gate_set_valid(&ctx->avsync_gate);
                    ALOGI("✅ AVSYNC: After firstFrameAfterSeek → switching master VIDEO → AUDIO (audio is playing)");
                } else {
                    // Audio не играет или stalled → остаёмся на VIDEO master
                    ALOGI("⚠️ AVSYNC: After firstFrameAfterSeek → staying on VIDEO master (audio not playing or stalled)");
                }
            }
            
            // Восстанавливаем master clock в зависимости от наличия аудио
            if (ctx->has_audio == 1) {
                // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - проверяем clock_valid перед использованием audio master
                if (ctx->audio && ctx->audio->clock_valid && !ctx->audio->track_failed) {
                    avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_AUDIO_GATE);
                    // AVSYNC станет валидным когда audio начнёт играть
                } else {
                    // Audio clock невалиден → используем VIDEO master
                    avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
                    avsync_gate_set_valid(&ctx->avsync_gate);
                    ALOGI("🔓 SEEK DONE: AVSYNC restored (VIDEO master - audio clock invalid)");
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - возобновляем audio после seek
                if (ctx->audio) {
                    extern void audio_resume(AudioState *as);
                    audio_resume(ctx->audio);
                    
                    // Эмитим audioState событие
                    extern void native_player_emit_audio_state_event(const char *state);
                    native_player_emit_audio_state_event("PLAYING");
                    
                    ALOGI("🔓 SEEK DONE: Audio resumed");
                }
            } else {
                avsync_gate_set_master(&ctx->avsync_gate, AVSYNC_MASTER_VIDEO_GATE);
                avsync_gate_set_valid(&ctx->avsync_gate);
            }
            
            ALOGI("🎯 firstFrameAfterSeek emitted AFTER swapBuffers, AVSYNC restored");
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - останавливаем seek watchdog
            extern void seek_watchdog_stop(PlayerContext *ctx);
            seek_watchdog_stop(ctx);
        }
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: POWER_SAVE/APS_ASSERT - обновляем FPS счетчик
    if (g_fps_window_start_ms == 0) {
        g_fps_window_start_ms = current_ms;
        g_frame_counter = 0;
    }
    g_frame_counter++;
    if (current_ms - g_fps_window_start_ms >= 1000) {
        g_last_fps = g_frame_counter;
        g_frame_counter = 0;
        g_fps_window_start_ms = current_ms;
        ALOGD("🎞️ Render FPS: %d", g_last_fps);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: RENDER_STALL_ASSERT - обновляем last_render_ts_ms ПОСЛЕ каждого успешного eglSwapBuffers
    // Это heartbeat рендера - используется для проверки, что кадры реально обновляются
    // Используем monotonic time (av_gettime_relative) для точности
    extern PlayerContext *g_player_context;
    if (g_player_context) {
        g_player_context->last_render_ts_ms = av_gettime_relative() / 1000;  // Конвертируем микросекунды в миллисекунды
    }
    
    // 🔒 FIX Z25: Эмитим first_frame event ПОСЛЕ eglSwapBuffers()
    // Это критично для скрытия loader в UI - loader скрывается ТОЛЬКО после реального рендера первого кадра
    // prepared ≠ first frame - prepared означает metadata OK, first_frame означает кадр на экране
    if (!vr->first_frame_rendered) {
        vr->first_frame_rendered = 1;
        extern void native_player_emit_first_frame_event(void);
        native_player_emit_first_frame_event();
        ALOGI("✅ First frame rendered and event emitted");
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC IMPLEMENTATION - ШАГ 4
    // 🔥 КАДР РЕАЛЬНО ПОКАЗАН ПОЛЬЗОВАТЕЛЮ
    // video_clock_pts = PTS ПОКАЗАННОГО КАДРА (с fallback на frame_timer)
    // PTS обязателен, если pts == NAN → используем fallback
    // Примечание: video clock обновляется в video_clock_on_frame_render() после eglSwapBuffers
    // Здесь только обновляем PlayerContext для обратной совместимости
    // Примечание: в этой функции параметр называется `frame`, а не `frame0`
    extern PlayerContext *g_player_context;
    if (frame && g_player_context) {
        // Вычисляем pts0 из frame для обновления PlayerContext
        double pts0 = frame_pts_sec(frame, vr->time_base);
        if (isnan(pts0)) {
            // Fallback: используем frame->pts, если доступен
            if (frame->pts != AV_NOPTS_VALUE) {
                pts0 = frame->pts * av_q2d(vr->time_base);
            }
            // Если pts0 всё ещё NAN, обновление clock выполнится в video_clock_on_frame_render()
        }
        
        // Обновляем PlayerContext для обратной совместимости (только если pts0 валиден)
        if (!isnan(pts0) && pts0 >= 0.0) {
            PlayerContext *ctx = g_player_context;
            ctx->master_clock_ms = (int64_t)(pts0 * 1000.0);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - обновляем video clock в AVSyncGate ПОСЛЕ eglSwapBuffers
            int64_t clock_us = (int64_t)(pts0 * 1000000.0);
            avsync_gate_update_video_clock(&ctx->avsync_gate, clock_us);
            
            // Обновляем avsync.video_clock
            if (ctx->avsync.master == CLOCK_MASTER_VIDEO || 
                ctx->avsync.master == CLOCK_MASTER_AUDIO) {
                ctx->avsync.video_clock = pts0;  // Используем pts0 вместо updated_video_clock
            }
        }
    }
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    // Шаг 35.4: Zero-copy safety - frame НЕ сохраняется после вызова
    // decoder thread может освободить frame сразу после возврата
    
    return 0;
}

/// Рендерить кадр(ы) с interpolation (Шаг 41.2, 41.3, 41.4)
int video_render_gl_draw(VideoRenderGL *vr, AVFrame *frame0, AVFrame *frame1, double alpha) {
    if (!vr || !frame0) {
        return -1;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    if (vr->state != VR_STATE_READY && vr->state != VR_STATE_RENDERING) {
        pthread_mutex_unlock(&vr->render_mutex);
        return -1;
    }
    
    // 🔴 КРИТИЧНО: Делаем EGL context текущим ТОЛЬКО один раз (оптимизация через guard)
    // ШАГ 11.2: Оптимизация eglMakeCurrent (guard) - избегаем лишних вызовов
    if (!vr->egl_current) {
        if (vr->egl_context == EGL_NO_CONTEXT) {
            ALOGE("❌ Cannot make EGL context current: context is EGL_NO_CONTEXT");
            pthread_mutex_unlock(&vr->render_mutex);
            return -1;
        }
        
        // 🔴 КРИТИЧНО: Для ImageTexture используем EGL_NO_SURFACE, для Surface - egl_surface
        EGLSurface target_surface = (vr->render_target == RENDER_TARGET_IMAGE_TEXTURE) 
            ? EGL_NO_SURFACE 
            : vr->egl_surface;
        
        // Для ImageTexture не проверяем egl_surface
        if (vr->render_target == RENDER_TARGET_SURFACE) {
            if (vr->egl_surface == EGL_NO_SURFACE) {
                ALOGE("❌ Cannot make EGL context current: surface is EGL_NO_SURFACE");
                pthread_mutex_unlock(&vr->render_mutex);
                return -1;
            }
        }
        
        EGLBoolean result = eglMakeCurrent(vr->egl_display, target_surface, target_surface, vr->egl_context);
        if (!result) {
            EGLint error = eglGetError();
            ALOGE("❌ Failed to make EGL context current: EGL error 0x%x (EGL_BAD_ACCESS=0x3002)", error);
            ALOGE("   display=%p, surface=%p, context=%p, target=%d", 
                  (void *)vr->egl_display, (void *)target_surface, (void *)vr->egl_context, vr->render_target);
            pthread_mutex_unlock(&vr->render_mutex);
            return -1;
        }
        vr->egl_current = true;
        ALOGD("✅ EGL context made current in render thread (target=%d)", vr->render_target);
    }
    
    vr->state = VR_STATE_RENDERING;
    
    // 🔴 ЭТАЛОН: Используем viewport из Flutter (размер экрана), а не размер surface
    // Если viewport не установлен - используем размер surface как fallback
    int viewport_w = vr->viewport_w > 0 ? vr->viewport_w : 0;
    int viewport_h = vr->viewport_h > 0 ? vr->viewport_h : 0;
    
    if (viewport_w == 0 || viewport_h == 0) {
        // Fallback: получаем размеры surface
        EGLint surface_w = 0, surface_h = 0;
        if (vr->egl_surface != EGL_NO_SURFACE) {
            eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_WIDTH, &surface_w);
            eglQuerySurface(vr->egl_display, vr->egl_surface, EGL_HEIGHT, &surface_h);
        }
        viewport_w = surface_w;
        viewport_h = surface_h;
    }
    
    // 🔴 ЭТАЛОН: glViewport всегда на весь экран (aspect ratio управляется через uniform в vertex shader)
    if (viewport_w > 0 && viewport_h > 0) {
        glViewport(0, 0, viewport_w, viewport_h);
    } else {
        // Fallback на размеры видео (не должно происходить)
        ALOGW("⚠️ Cannot get viewport size, using video size as fallback");
        glViewport(0, 0, frame0->width, frame0->height);
    }
    
    // Шаг 41.4: Загружаем frame0 в текстуры (ШАГ 11.1 - исправлено)
    upload_yuv_frame(vr, vr->tex_y0, vr->tex_u0, vr->tex_v0, frame0, vr->video_width, vr->video_height);
    
    // Шаг 41.4: Загружаем frame1 в текстуры (если есть)
    bool has_next = (frame1 != NULL);
    if (has_next) {
        upload_yuv_frame(vr, vr->tex_y1, vr->tex_u1, vr->tex_v1, frame1, vr->video_width, vr->video_height);
    }
    vr->has_next_frame = has_next;
    
    // Используем shader program
    glUseProgram(vr->shader_program);
    
    // 🔴 ШАГ 6: Передаём scale в shader (делается при каждом resize/rotate, не каждый frame)
    if (vr->uniforms.uScaleX >= 0 && vr->uniforms.uScaleY >= 0) {
        glUniform1f(vr->uniforms.uScaleX, vr->scale_x);
        glUniform1f(vr->uniforms.uScaleY, vr->scale_y);
    } else {
        ALOGW("⚠️ ШАГ 6: uScaleX or uScaleY uniform not found (shader may not support aspect ratio)");
    }
    
    // 🔴 ЭТАЛОН: Bind текстур для простого shader (texY, texU, texV)
    // Для сложного shader используй tex_y0/u0/v0
    GLint texY_loc = glGetUniformLocation(vr->shader_program, "texY");
    GLint texU_loc = glGetUniformLocation(vr->shader_program, "texU");
    GLint texV_loc = glGetUniformLocation(vr->shader_program, "texV");
    
    // ✅ ШАГ 6.6: Убираем log-spam - логируем только в debug режиме
    #ifdef VIDEO_RENDER_DEBUG
    ALOGI("🔍 Shader uniforms: texY=%d, texU=%d, texV=%d", texY_loc, texU_loc, texV_loc);
    #endif
    if (texY_loc < 0 || texU_loc < 0 || texV_loc < 0) {
        ALOGE("❌ CRITICAL: Invalid uniform locations for simple shader (texY/texU/texV)");
        ALOGE("   This means shader compilation failed or wrong shader is used");
    }
    
    // Если простой shader (эталонный) - используем texY/texU/texV
    if (texY_loc >= 0 && texU_loc >= 0 && texV_loc >= 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vr->tex_y0);
        glUniform1i(texY_loc, 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, vr->tex_u0);
        glUniform1i(texU_loc, 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vr->tex_v0);
        glUniform1i(texV_loc, 2);
    } else {
        // Сложный shader (с интерполяцией) - используем кешированные uniform locations
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, vr->tex_y0);
        if (vr->uniforms.tex_y0 >= 0) glUniform1i(vr->uniforms.tex_y0, 0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, vr->tex_u0);
        if (vr->uniforms.tex_u0 >= 0) glUniform1i(vr->uniforms.tex_u0, 1);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, vr->tex_v0);
        if (vr->uniforms.tex_v0 >= 0) glUniform1i(vr->uniforms.tex_v0, 2);
    }
    
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, has_next ? vr->tex_y1 : vr->tex_y0);
    if (vr->uniforms.tex_y1 >= 0) glUniform1i(vr->uniforms.tex_y1, 3);
    
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, has_next ? vr->tex_u1 : vr->tex_u0);
    if (vr->uniforms.tex_u1 >= 0) glUniform1i(vr->uniforms.tex_u1, 4);
    
    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, has_next ? vr->tex_v1 : vr->tex_v0);
    if (vr->uniforms.tex_v1 >= 0) glUniform1i(vr->uniforms.tex_v1, 5);
    
    // ШАГ 11.1: Используем кешированные interpolation uniforms
    if (vr->uniforms.uAlpha >= 0) {
        // 🔴 ШАГ 8: Защита от NaN/Inf (КРИТИЧНО для стабильности)
        float clamped_alpha = (float)alpha;
        if (isnan(clamped_alpha) || isinf(clamped_alpha)) {
            ALOGE("❌ Alpha is NaN/Inf in video_render_gl_draw: %.3f, forcing to 0.0", clamped_alpha);
            clamped_alpha = 0.0f;
        } else {
            // Clamp alpha в [0..1]
            if (clamped_alpha < 0.0f) clamped_alpha = 0.0f;
            if (clamped_alpha > 1.0f) clamped_alpha = 1.0f;
        }
        glUniform1f(vr->uniforms.uAlpha, clamped_alpha);
    }
    if (vr->uniforms.uHasNextFrame >= 0) {
        // 🔴 ШАГ 8: has_next должен быть false, если frame1 == NULL
        // Это гарантирует, что shader использует только frame0
        glUniform1i(vr->uniforms.uHasNextFrame, has_next ? 1 : 0);
    }
    
    // ШАГ 11.1: Используем кешированные colorspace uniforms
    VideoColorInfo color_info;
    video_color_info_from_frame(frame0, &color_info);
    
    if (vr->uniforms.u_colorspace >= 0) {
        glUniform1i(vr->uniforms.u_colorspace, video_color_info_get_colorspace_index(&color_info));
    }
    if (vr->uniforms.u_range >= 0) {
        glUniform1i(vr->uniforms.u_range, video_color_info_get_range_index(&color_info));
    }
    if (vr->uniforms.u_is_hdr >= 0) {
        glUniform1i(vr->uniforms.u_is_hdr, video_color_info_is_hdr(&color_info) ? 1 : 0);
    }
    
    // Resize/Rotation: Вычисляем и применяем transform matrix
    if (vr->uniforms.uTransform >= 0) {
        float transform_mat[16];
        compute_transform(vr, transform_mat);
        glUniformMatrix4fv(vr->uniforms.uTransform, 1, GL_FALSE, transform_mat);
    }
    if (vr->uniforms.uRotation >= 0) {
        glUniform1i(vr->uniforms.uRotation, vr->layout.rotation);
    }
    
    // Gestures: Применяем scale и pan
    if (vr->uniforms.uGestureScale >= 0) {
        glUniform1f(vr->uniforms.uGestureScale, vr->transform.scale);
    }
    if (vr->uniforms.uGestureOffset >= 0) {
        glUniform2f(vr->uniforms.uGestureOffset, vr->transform.offset_x, vr->transform.offset_y);
    }
    
    // Устанавливаем vertex attributes
    GLint pos_loc = glGetAttribLocation(vr->shader_program, "aPosition");
    GLint tex_loc = glGetAttribLocation(vr->shader_program, "aTexCoord");
    
    glBindBuffer(GL_ARRAY_BUFFER, vr->vbo);
    glEnableVertexAttribArray(pos_loc);
    glVertexAttribPointer(pos_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(tex_loc);
    glVertexAttribPointer(tex_loc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
    
    // Очищаем экран
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // 🔴 ШАГ 5: Минимальный эталон YUV → RGB (без FBO, без ImageTexture)
    // SurfaceTexture - рендерим напрямую в EGLSurface
    // Временно выключены: interpolation, dual frame, jitter buffer, HDR/colorspace
    
    // Рисуем quad в egl_surface
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    
    // 🔴 ШАГ 5: ЕДИНСТВЕННОЕ МЕСТО для eglSwapBuffers при рендере кадра (interpolation)
    // eglSwapBuffers = показ КАДРА, нет кадра → нет swap
    // eglSwapBuffers уведомляет Flutter автоматически через SurfaceTexture
    EGLBoolean swap_result_interp = eglSwapBuffers(vr->egl_display, vr->egl_surface);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.3
    // Обновляем clock после eglSwapBuffers (interpolation path)
    // Примечание: frame0 - это параметр функции video_render_gl_draw, используем его
    extern PlayerContext *g_player_context;
    if (g_player_context && g_player_context->video && frame0) {
        extern void video_clock_on_frame_render(VideoState *vs, AVFrame *frame);
        video_clock_on_frame_render(g_player_context->video, frame0);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: EGL_CONTEXT_LOST ASSERT - проверка после eglSwapBuffers
    EGLint egl_err_interp = eglGetError();
    if (egl_err_interp == EGL_CONTEXT_LOST || egl_err_interp == EGL_BAD_CONTEXT) {
        ALOGE("❌ EGL_CONTEXT_LOST detected (err=0x%x)", egl_err_interp);
        extern void native_player_emit_egl_context_lost_event(void);
        native_player_emit_egl_context_lost_event();
        pthread_mutex_unlock(&vr->render_mutex);
        return -1;  // render loop должен остановиться
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: VSYNC_DROP_DETECT - обновляем счетчики ПОСЛЕ каждого успешного eglSwapBuffers
    int64_t current_ms_interp = now_ms();
    if (g_swap_count == 0) {
        g_first_swap_time = current_ms_interp / 1000.0;  // В секундах
    }
    g_swap_count++;
    g_last_swap_ts_ms = current_ms_interp;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: POWER_SAVE/APS_ASSERT - обновляем FPS счетчик
    if (g_fps_window_start_ms == 0) {
        g_fps_window_start_ms = current_ms_interp;
        g_frame_counter = 0;
    }
    g_frame_counter++;
    if (current_ms_interp - g_fps_window_start_ms >= 1000) {
        g_last_fps = g_frame_counter;
        g_frame_counter = 0;
        g_fps_window_start_ms = current_ms_interp;
        ALOGD("🎞️ Render FPS: %d", g_last_fps);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: RENDER_STALL_ASSERT - обновляем last_render_ts_ms ПОСЛЕ каждого успешного eglSwapBuffers
    // Это heartbeat рендера - используется для проверки, что кадры реально обновляются
    extern PlayerContext *g_player_context;
    if (g_player_context) {
        g_player_context->last_render_ts_ms = av_gettime_relative() / 1000;  // Конвертируем микросекунды в миллисекунды
    }
    
    // 🔒 FIX Z25: Эмитим first_frame event ПОСЛЕ eglSwapBuffers()
    // Это критично для скрытия loader в UI - loader скрывается ТОЛЬКО после реального рендера первого кадра
    // prepared ≠ first frame - prepared означает metadata OK, first_frame означает кадр на экране
    if (!vr->first_frame_rendered) {
        vr->first_frame_rendered = 1;
        extern void native_player_emit_first_frame_event(void);
        native_player_emit_first_frame_event();
        ALOGI("✅ First frame rendered and event emitted (interpolation)");
    }
    
    // Шаг 41.9: Субтитры рисуются ПОСЛЕ видео (не участвуют в interpolation)
    // Вызывается из render loop с audio_clock
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    return 0;
}

/// Включить/выключить interpolation (Шаг 41.2)
void video_render_gl_set_interpolation(VideoRenderGL *vr, bool enabled) {
    if (!vr) {
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    vr->interpolation_enabled = enabled;
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("Interpolation %s", enabled ? "enabled" : "disabled");
}

/// Установить режим interpolation (Шаг 41.8)
///
/// @param vr Видеорендер
/// @param mode Режим (INTERP_AUTO, INTERP_FORCE_ON, INTERP_FORCE_OFF)
void video_render_gl_set_interp_mode(VideoRenderGL *vr, int mode) {
    if (!vr) {
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    vr->interp_mode = mode;
    
    // Принудительно устанавливаем enabled в зависимости от режима
    if (mode == 1) { // INTERP_FORCE_ON
        vr->interpolation_enabled = true;
    } else if (mode == 2) { // INTERP_FORCE_OFF
        vr->interpolation_enabled = false;
    }
    // mode == 0 (INTERP_AUTO) - interpolation_enabled управляется автоматически
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("Interpolation mode set to: %d", mode);
}

void video_render_gl_subtitle(VideoRenderGL *vr, const char *subtitle_text, double audio_clock) {
    // TODO: Реализовать рендеринг субтитров поверх видео (Шаг 28.7)
    // Это можно сделать через:
    // 1. OpenGL text rendering (сложно)
    // 2. Flutter overlay (проще, через MethodChannel)
    // Пока оставляем пустым - субтитры рендерятся во Flutter
    (void)vr;
    (void)subtitle_text;
    (void)audio_clock;
}

void video_render_gl_clear(VideoRenderGL *vr, double seek_target) {
    if (!vr || !vr->initialized) {
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    if (vr->state != VR_STATE_READY && vr->state != VR_STATE_RENDERING) {
        pthread_mutex_unlock(&vr->render_mutex);
        return;
    }
    
    // ШАГ 11.2: Оптимизация eglMakeCurrent (guard)
    if (!vr->egl_current) {
        // 🔴 КРИТИЧНО: Для ImageTexture используем EGL_NO_SURFACE
        EGLSurface target_surface = (vr->render_target == RENDER_TARGET_IMAGE_TEXTURE) 
            ? EGL_NO_SURFACE 
            : vr->egl_surface;
        
        if (vr->render_target == RENDER_TARGET_SURFACE && vr->egl_surface == EGL_NO_SURFACE) {
            pthread_mutex_unlock(&vr->render_mutex);
            return;
        }
        
        if (!eglMakeCurrent(vr->egl_display, target_surface, target_surface, vr->egl_context)) {
            pthread_mutex_unlock(&vr->render_mutex);
            return;
        }
        vr->egl_current = true;
    }
    
    // Шаг 35.7: Очищаем экран (при seek) - только при clear, не каждый кадр
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    // 🔴 ЭТАЛОН: SurfaceTexture - ВСЕГДА вызываем eglSwapBuffers
    eglSwapBuffers(vr->egl_display, vr->egl_surface);
    
    // ШАГ 11.1: Сброс persistent textures при clear
    vr->textures_initialized = false;
    vr->tex_w = 0;
    vr->tex_h = 0;
    
    // Освобождаем последний кадр при clear
    if (vr->last_frame) {
        av_frame_free(&vr->last_frame);
        vr->last_frame = NULL;
    }
    
    // ШАГ 6: Сброс статистики при seek
    memset(&vr->interp_stats, 0, sizeof(vr->interp_stats));
    vr->interp_stats.toggle_cooldown = 0; // ШАГ 6.5: Сброс cooldown
    vr->interpolation_enabled = false; // Отключаем interpolation после seek
    vr->has_next_frame = false; // Interpolation запрещена, пока не появятся 2 стабильных кадра
    
    // ШАГ 8: Сброс alpha smoothing при seek
    vr->interp_alpha.alpha_valid = false;
    vr->interp_alpha.last_alpha = 0.0f;
    
    // 🔴 ШАГ 4: Сброс jitter buffer при seek (чтобы снова ждать накопления кадров)
    vr->jitter_buffer_ready = false;
    
    // 🔴 ЭТАЛОН: Сброс флагов первого кадра при seek (критично для правильного seek)
    // ⛔ БЕЗ ЭТОГО первый кадр после seek не инициализирует clock правильно
    vr->first_frame_rendered = false;
    vr->clock_initialized = false;
    // 🔴 ЭТАЛОН: Сброс video-only clock при seek (ШАГ I + ШАГ J)
    // ⛔ НЕ на 0.0, а на seek_target - это убирает ускорение и скачок таймлайна
    if (seek_target > 0.0) {
        vr->video_clock = seek_target;
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE FIX - PATCH 3: УДАЛЕНО frame_timer usage
        // frame_timer больше не используется для clock
        ALOGI("🔍 ШАГ J: video_clock reset to seek_target=%.3f", seek_target);
    } else {
        // Полный сброс (не seek, а init/reset)
        vr->video_clock = 0.0;
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE FIX - PATCH 3: УДАЛЕНО frame_timer usage
    }
    
    // Gestures: Сброс transform при seek (опционально, можно оставить состояние)
    // vr->transform.scale = 1.0f;
    // vr->transform.offset_x = 0.0f;
    // vr->transform.offset_y = 0.0f;
    
    // ШАГ 11.1: Сброс persistent textures при clear
    vr->textures_initialized = false;
    vr->tex_w = 0;
    vr->tex_h = 0;
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("✅ video_render_gl_clear: All flags reset (jitter_buffer, first_frame, clock)");
}

/// 🔴 ЭТАЛОН: Обновить aspect ratio scale factors (вызывается при изменении surface/video size/fit mode)
void video_render_gl_update_aspect(VideoRenderGL *vr) {
    if (!vr || vr->video_width <= 0 || vr->video_height <= 0) {
        return;
    }
    
    // 🔴 ШАГ 4: Используем размеры surface для вычисления aspect ratio (не viewport!)
    // Surface размеры - это реальный размер EGLSurface (SurfaceTexture)
    int screen_w = vr->surface_w > 0 ? vr->surface_w : vr->video_width;
    int screen_h = vr->surface_h > 0 ? vr->surface_h : vr->video_height;
    
    if (screen_w <= 0 || screen_h <= 0) {
        // Fallback: scale = 1.0 (stretch)
        vr->scale_x = 1.0f;
        vr->scale_y = 1.0f;
        ALOGW("⚠️ Cannot compute aspect ratio: surface=%dx%d, video=%dx%d (using stretch)", 
              vr->surface_w, vr->surface_h, vr->video_width, vr->video_height);
        return;
    }
    
    float video_ratio = (float)vr->video_width / (float)vr->video_height;
    float surface_ratio = (float)screen_w / (float)screen_h;
    
    float sx = 1.0f, sy = 1.0f;
    
    // 🔴 ШАГ 4: ЭТАЛОН расчёт scale в зависимости от fit mode (как в VLC/ExoPlayer)
    switch (vr->fit_mode) {
        case FIT_CONTAIN: // FIT (letterbox, без обрезки)
            if (surface_ratio > video_ratio) {
                sx = video_ratio / surface_ratio;
                sy = 1.0f;
            } else {
                sx = 1.0f;
                sy = surface_ratio / video_ratio;
            }
            break;
            
        case FIT_COVER: // FILL (fullscreen, crop)
            if (surface_ratio > video_ratio) {
                sx = 1.0f;
                sy = surface_ratio / video_ratio;
            } else {
                sx = video_ratio / surface_ratio;
                sy = 1.0f;
            }
            break;
            
        case FIT_STRETCH: // stretch - растянуть (искажает)
            sx = 1.0f;
            sy = 1.0f;
            break;
            
        case FIT_ORIGINAL: // original - 1:1 пиксели
            sx = (float)vr->video_width / (float)screen_w;
            sy = (float)vr->video_height / (float)screen_h;
            break;
    }
    
    vr->scale_x = sx;
    vr->scale_y = sy;
    
    ALOGI("✅ ШАГ 4: Aspect ratio updated: fit_mode=%d, video=%dx%d (ratio=%.3f), surface=%dx%d (ratio=%.3f), scale=%.3fx%.3f",
          vr->fit_mode, vr->video_width, vr->video_height, video_ratio,
          screen_w, screen_h, surface_ratio, sx, sy);
}

/// 🔴 ЭТАЛОН: Установить fit mode (contain/cover/stretch/original)
void video_render_gl_set_fit_mode(VideoRenderGL *vr, int fit_mode) {
    if (!vr) {
        return;
    }
    
    if (fit_mode < 0 || fit_mode > 3) {
        ALOGW("⚠️ Invalid fit_mode: %d, using FIT_CONTAIN", fit_mode);
        fit_mode = FIT_CONTAIN;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    vr->fit_mode = fit_mode;
    video_render_gl_update_aspect(vr);
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("✅ video_render_gl_set_fit_mode: mode=%d (scale=%.3fx%.3f)", 
          fit_mode, vr->scale_x, vr->scale_y);
}

void video_render_gl_set_paused(VideoRenderGL *vr, bool paused) {
    if (!vr) {
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    vr->paused = paused;
    
    // ШАГ 8: Сброс alpha smoothing при паузе
    if (paused) {
        vr->interp_alpha.alpha_valid = false;
        vr->interp_alpha.last_alpha = 0.0f;
    } else {
        // 🔴 ЗАДАЧА 1: При resume сбрасываем alpha smoothing для плавного возврата
        vr->interp_alpha.alpha_valid = false;
        vr->interp_alpha.last_alpha = 0.0f;
    }
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGD("Video render paused: %s", paused ? "true" : "false");
}

/// 🔴 ШАГ 4: Установить флаг готовности плеера
///
/// Вызывается из JNI когда decoder запущен и первый кадр готов.
/// Без этого флага render loop не будет рендерить кадры.
void video_render_gl_set_prepared(VideoRenderGL *vr, bool prepared) {
    if (!vr) {
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    vr->player_prepared = prepared;
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGI("🔴 ШАГ 4: Player prepared flag set: %s", prepared ? "true" : "false");
}

/// 🔴 ШАГ 5: Вспомогательная функция для ожидания (БЕЗ swap)
/// 
/// ❌ НЕЛЬЗЯ делать eglSwapBuffers без кадра - это ломает тайминг
/// VSync ожидание происходит ТОЛЬКО через eglSwapBuffers при реальном рендере
static void wait_for_vsync(VideoRenderGL *vr) {
    // НИЧЕГО не делаем
    // VSync ожидание происходит ТОЛЬКО через eglSwapBuffers при реальном рендере
    usleep(1000); // 1ms — мягкий yield CPU
}

// 🔴 УДАЛЕНО: mark_frame_available больше не нужен для SurfaceTexture
// SurfaceTexture автоматически уведомляет Flutter через eglSwapBuffers

/// VSync-driven render loop (Шаг 33.6, 35.6, 41.9)
void video_render_gl_render_loop(VideoRenderGL *vr,
                                  struct FrameQueue *frame_queue,
                                  struct AudioState *audio_state,
                                  struct VideoState *video_state,  // Шаг 41.9: для subtitle_manager
                                  int *abort) {
    if (!vr || !frame_queue || !abort) {
        return;
    }
    
    VideoState *vs = (VideoState *)video_state; // Для удобства, приведение типа
    
    // 🔴 КРИТИЧНО: Делаем EGL context текущим в render thread ПЕРЕД любыми GL вызовами
    // БЕЗ этого все gl*() вызовы будут падать с "call to OpenGL ES API with no current context"
    // Это ОБЯЗАТЕЛЬНО, так как EGLContext был отвязан от JNI thread после init
    
    // 🔴 КРИТИЧНО: Для ImageTexture EGL surface не нужен, только context
    if (vr->egl_context == EGL_NO_CONTEXT) {
        ALOGE("❌ Cannot start render loop: EGL context is invalid");
        ALOGE("   context=%p", (void *)vr->egl_context);
        return;
    }
    
    // 🔴 ЭТАЛОН: Проверяем, что render_target установлен
    if (vr->render_target == RENDER_TARGET_NONE) {
        ALOGE("❌ Cannot start render loop: Render target not set yet (call video_render_gl_attach_window or video_render_gl_register_image_texture first)");
        return;
    }
    
    // 🔴 ЭТАЛОН: SurfaceTexture - ВСЕГДА используем EGLSurface
    if (vr->egl_surface == EGL_NO_SURFACE) {
        ALOGE("❌ Cannot start render loop: EGL surface is invalid");
        ALOGE("   surface=%p, context=%p", (void *)vr->egl_surface, (void *)vr->egl_context);
        return;
    }
    
    // 🔍 ДИАГНОСТИКА: Проверяем, не current ли context уже в другом потоке
    EGLContext current_before = eglGetCurrentContext();
    if (current_before != EGL_NO_CONTEXT && current_before != vr->egl_context) {
        ALOGW("⚠️ EGL context is current in another thread: %p (expected EGL_NO_CONTEXT or %p)",
              (void *)current_before, (void *)vr->egl_context);
        ALOGW("   This may cause EGL_BAD_ACCESS. Context should be detached from JNI thread before render loop starts.");
    }
    
    // 🔴 ЭТАЛОН: SurfaceTexture - ВСЕГДА используем egl_surface
    EGLSurface target_surface = vr->egl_surface;
    
    // 🔴 ЭТАЛОН: Делаем context current в render thread БЕЗ проверок "если уже current"
    // Context должен быть detached из JNI thread перед этим
    EGLBoolean egl_result = eglMakeCurrent(vr->egl_display, target_surface, target_surface, vr->egl_context);
    if (!egl_result) {
        EGLint error = eglGetError();
        ALOGE("❌ Failed to make EGL context current in render loop: EGL error 0x%x (EGL_BAD_ACCESS=0x3002)", error);
        ALOGE("   display=%p, surface=%p, context=%p", 
              (void *)vr->egl_display, (void *)target_surface, (void *)vr->egl_context);
        ALOGE("   current_before=%p, render_target=%d", (void *)current_before, vr->render_target);
        
        // 🔴 ДИАГНОСТИКА: Если EGL_BAD_ACCESS, значит context был current в другом потоке
        if (error == 0x3002) {
            ALOGE("   🔴 EGL_BAD_ACCESS detected: Context was current in another thread!");
            ALOGE("   🔴 Check that eglMakeCurrent(EGL_NO_CONTEXT) was called in JNI thread before render_loop_start()");
            ALOGE("   🔴 Also check video_render_gl_register_image_texture() - it must detach context after FBO creation");
        }
        return;
    }
    
    // Проверяем, что context действительно current
    EGLContext current_ctx = eglGetCurrentContext();
    if (current_ctx != vr->egl_context) {
        ALOGE("❌ EGL context mismatch: expected %p, got %p", (void *)vr->egl_context, (void *)current_ctx);
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return;
    }
    
    vr->egl_current = true;
    ALOGI("✅ EGL context made current in render thread: context=%p (was %p before)", 
          (void *)vr->egl_context, (void *)current_before);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Устанавливаем VSYNC ПОСЛЕ каждого eglMakeCurrent
    // eglSwapInterval может сброситься после некоторых операций, поэтому устанавливаем его каждый раз
    EGLBoolean swap_interval_ok = eglSwapInterval(vr->egl_display, 1);
    if (!swap_interval_ok) {
        EGLint error = eglGetError();
        ALOGW("⚠️ Failed to set eglSwapInterval(1) in render loop: EGL error 0x%x", error);
    } else {
        EGLint interval = 0;
        eglQueryContext(vr->egl_display, vr->egl_context, EGL_CONTEXT_CLIENT_VERSION, &interval);
        ALOGI("✅ VSYNC enabled: eglSwapInterval(1) set in render thread");
    }
    
    // 🔥 КРИТИЧЕСКИЙ ASSERT: Проверяем, что EGL context действительно current
    EGLContext verify_ctx = eglGetCurrentContext();
    if (verify_ctx != vr->egl_context) {
        ALOGE("❌ FATAL: EGL context NOT current after eglMakeCurrent! expected=%p, got=%p", 
              (void *)vr->egl_context, (void *)verify_ctx);
        // Это критическая ошибка - render loop не может работать без current context
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Эмитим surfaceReady ПОСЛЕ успешного eglMakeCurrent
    // Это критично для TEXTURE-RACE fix - Flutter должен знать, что EGLSurface готов
    // AVSYNC-GATE открывается только после surfaceReady
    extern void native_player_emit_surface_ready_event(void);
    native_player_emit_surface_ready_event();
    ALOGI("✅ surfaceReady event emitted (EGLSurface ready, AVSYNC-GATE will open)");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Открываем AVSYNC-GATE после успешного eglMakeCurrent
    // Clocks и decode стартуют ТОЛЬКО после gate открыт
    // Это гарантирует, что первый frame не будет dropped из-за race condition
    if (vs && vs->player_ctx) {
        PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
        ctx->avsync_gate_open = 1;
        ALOGI("✅ AVSYNC-GATE OPENED (clocks and decode can start now)");
        
        // 🔥 КРИТИЧЕСКИЙ FIX: DECODE-AUTO-START - запускаем decode автоматически после surfaceReady
        // Это решает FIRST-FRAME-DEADLOCK: decode стартует БЕЗ необходимости play()
        // play() теперь управляет ТОЛЬКО clock/pause, а не запуском decode
        // Правильная модель: surfaceReady → decode auto-start → firstFrame → ready → play() (clock start)
        if (!ctx->decode_started) {
            ctx->decode_started = 1;
            ctx->state.abort_request = 0;
            
            // Запускаем demux thread
            extern void *demux_thread(void *arg);
            int ret_demux = pthread_create(&ctx->demuxThread, NULL, demux_thread, ctx);
            if (ret_demux != 0) {
                ALOGE("❌ DECODE-AUTO-START: Failed to create demux thread after AVSYNC-GATE open: %d", ret_demux);
                ctx->decode_started = 0;
            } else {
                ALOGI("✅ DECODE-AUTO-START: Demux thread started after AVSYNC-GATE open (auto-start for first frame)");
                
                // Запускаем decode thread
                if (ctx->video) {
                    extern int video_decode_thread_start(VideoState *vs, AudioState *as);
                    extern void native_player_emit_decode_started_event(void);
                    int ret_decode = video_decode_thread_start(ctx->video, ctx->audio);
                    if (ret_decode < 0) {
                        ALOGE("❌ DECODE-AUTO-START: Failed to start decode thread after AVSYNC-GATE open: %d", ret_decode);
                        ctx->decode_started = 0;
                    } else {
                        ALOGI("✅ DECODE-AUTO-START: Decode thread started after AVSYNC-GATE open (auto-start for first frame)");
                        // 🔥 КРИТИЧЕСКИЙ FIX: DECODE_STARTED_ASSERT - эмитим decodeStarted после успешного старта
                        native_player_emit_decode_started_event();
                    }
                }
            }
        } else if (ctx->pending_play && ctx->play_requested) {
            // 🔥 КРИТИЧЕСКИЙ FIX: Если decode уже стартовал, но был pending play - сбрасываем pending
            // play() будет обработан в nativePlay() для управления clock
            ctx->pending_play = 0;
            ALOGI("✅ DECODE-AUTO-START: Decode already started, pending play cleared (play() will manage clock)");
        }
    }
    
    // 🔴 ШАГ 8: Логируем состояние интерполяции при старте
    // В AUTO режиме интерполяция будет включена автоматически при наличии 2+ кадров
    const char *interp_status = "auto";
    if (vr->interp_mode == 1) {
        interp_status = "enabled";
    } else if (vr->interp_mode == 2) {
        interp_status = "disabled";
    }
    ALOGI("VSync-driven render loop started (interpolation: %s, mode: %d)", 
          interp_status, vr->interp_mode);
    
    while (!*abort) {
        // 🔥 КРИТИЧЕСКИЙ FIX: SEEK-GATE - drop frames во время seek
        // Это критично для scrub (10-30 seek/сек) и предотвращает отрисовку "грязных" кадров
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: Если seek в процессе - drop все кадры до первого >= target
            if (ctx->seek_in_progress) {
                // Проверяем кадр из очереди (если есть) перед рендером
                Frame *peek_frame = frame_queue_peek_ptr((FrameQueue *)frame_queue);
                if (peek_frame && peek_frame->frame) {
                    // 🔥 КРИТИЧЕСКИЙ FIX: Используем другое имя переменной, чтобы избежать конфликта с функцией frame_pts_sec()
                    double peek_frame_pts_sec = frame_pts_sec(peek_frame->frame, vr->time_base);
                    if (isnan(peek_frame_pts_sec)) {
                        peek_frame_pts_sec = peek_frame->pts; // Fallback на сохранённый PTS
                    }
                    
                    // 🔥 КРИТИЧЕСКИЙ FIX: Drop кадры до seek_target
                    if (!isnan(peek_frame_pts_sec) && peek_frame_pts_sec < ctx->seek_target_pts - 0.01) {
                        ALOGD("🔍 SEEK-GATE: dropping frame in render loop (pts=%.3f < target=%.3f)", 
                              peek_frame_pts_sec, ctx->seek_target_pts);
                        frame_queue_next((FrameQueue *)frame_queue); // Удаляем из очереди
                        continue; // Пропускаем рендер
                    }
                } else {
                    // Нет кадра - ждём
                    av_usleep(1000); // 1ms
                    continue;
                }
            }
            
            // Legacy проверка seeking флага (для обратной совместимости)
            pthread_mutex_lock(&ctx->state.seek_mutex);
            bool is_seeking = ctx->state.seek_req.seeking;
            pthread_mutex_unlock(&ctx->state.seek_mutex);
            
            if (is_seeking && !ctx->seek_in_progress) {
                // Seek выполняется (legacy) - ждём, не рендерим
                av_usleep(1000); // 1ms
                continue;
            }
        }
        
        // ⛔ ЖЁСТКИЙ ГЕЙТ: пока плеер не prepared — НИЧЕГО не рендерим
        // 🔴 ШАГ 4: Это главный фикс против ускорения и зависания
        // 🔴 ШАГ 5: НЕТ КАДРОВ → НЕТ РЕНДЕРА → НЕТ SWAP
        if (!vr->player_prepared) {
            usleep(2000); // 2ms
            continue;
        }
        
        // 🔴 ШАГ 4: Jitter buffer - ждём накопления кадров перед стартом рендеринга
        // Это убирает стартовые "пусто → рывок" и стабилизирует тайминг
        // Флаг сбрасывается при seek через video_render_gl_clear()
        // 🔴 ШАГ 2: VSync-safe - ждём через VSync, не busy-wait
        if (!vr->jitter_buffer_ready) {
            // Ждём накопления кадров через VSync (не busy-wait)
            // Проверяем только в начале каждого VSync цикла
            if (frame_queue_size((FrameQueue *)frame_queue) < JITTER_BUFFER_MIN) {
                // Ещё не накопилось - ждём
                // 🔴 ШАГ 5: НЕТ КАДРОВ → НЕТ РЕНДЕРА → НЕТ SWAP
                usleep(2000); // 2ms
                continue;
            }
            // Накопилось достаточно кадров - готовы к рендерингу
            vr->jitter_buffer_ready = true;
            ALOGI("✅ Jitter buffer ready: %d frames accumulated", 
                  frame_queue_size((FrameQueue *)frame_queue));
        }
        // 🔴 ШАГ 2: VSync-safe render loop - блокируем по VSync в каждом цикле
        // eglSwapBuffers с eglSwapInterval(1) блокирует до следующего VSync
        // Это гарантирует, что цикл не крутится в busy-wait
        
        // Шаг 33.8: Pause handling
        if (vr->paused) {
            if (vr->last_frame) {
                // Рендерим последний кадр
                // 🔴 КРИТИЧНО: video_render_gl_draw() уже вызывает markFrameAvailable() внутри
                video_render_gl_draw(vr, vr->last_frame, NULL, 0.0f);
            } else {
                // Нет кадра для паузы - просто ждём
                // 🔴 ШАГ 5: НЕТ КАДРОВ → НЕТ РЕНДЕРА → НЕТ SWAP
                usleep(2000); // 2ms
            }
            continue;
        }
        
        // 🔴 ШАГ 2: VSync-safe render loop - убраны busy-wait и логи
        // Render loop просыпается строго по VSync, не крутится в busy-loop
        
        // Шаг 34.4: Audio starvation guard (video-only safe)
        // 🔴 ШАГ 5: Audio ещё не стартовал → не рендерим, но и не swap'аем
        if (audio_state && !clock_is_active(&((AudioState *)audio_state)->clock)) {
            usleep(2000); // 2ms
            continue;
        }

        // 🔒 DIFF 1: Рендерим буферизованный первый кадр ПЕРЕД обычным рендером
        //
        // АРХИТЕКТУРНОЕ ОБОСНОВАНИЕ:
        // ExoPlayer: onRenderedFirstFrame() вызывается автоматически после первого swapBuffers
        //            Буферизация implicit (MediaCodec держит кадр до готовности Surface)
        // FFmpeg: explicit buffering первого кадра + гарантированный рендер
        //
        // Это гарантирует, что первый кадр не потеряется для AVI и коротких файлов
        // 🔒 DIFF 1: Явно эмитим firstFrame event после рендера буферизованного кадра
        if (vs && vs->first_frame_ready && !vs->first_frame_rendered) {
            ALOGI("🎬 Rendering FIRST FRAME explicitly (buffered safety-net)");
            
            // Рендерим буферизованный первый кадр
            video_render_gl_draw(vr, vs->first_frame, NULL, 0.0f);
            
            // 🔒 DIFF 1: Явно вызываем eglSwapBuffers для первого кадра
            // Это гарантирует, что кадр реально отрисован на экране
            eglSwapBuffers(vr->egl_display, vr->egl_surface);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: Обновляем master_clock_ms ПОСЛЕ eglSwapBuffers для первого кадра
            if (vs && vs->player_ctx && vs->first_frame) {
                PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
                double first_frame_pts = frame_pts_sec(vs->first_frame, vr->time_base);
                if (!isnan(first_frame_pts) && first_frame_pts >= 0.0) {
                    ctx->master_clock_ms = (int64_t)(first_frame_pts * 1000.0);
                }
            }
            
            // Помечаем как отрисованный (и в VideoState, и в VideoRenderGL)
            vs->first_frame_rendered = 1;
            vr->first_frame_rendered = 1;
            
            // 🔒 DIFF 1: Явно эмитим firstFrame event после swapBuffers
            // Это критично - без этого события FSM никогда не перейдет в ready
            extern void native_player_emit_first_frame_event(void);
            native_player_emit_first_frame_event();
            
            ALOGI("✅ First frame rendered and firstFrame event emitted");
            ALOGI("   (ExoPlayer equivalent: onRenderedFirstFrame() callback)");
            
            // Продолжаем обычный рендер после первого кадра
            // НЕ делаем continue - следующий кадр из очереди будет отрисован нормально
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.2: Background playback
        // В background режиме render loop НЕ активен
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            
            // Проверяем playback_mode
            if (ctx->playback_mode == MODE_AUDIO_ONLY) {
                // ⛔ Background mode - НЕ рендерим видео
                // ❌ НЕ делаем eglSwapBuffers
                // ❌ НЕ обновляем video_clock
                usleep(10000); // 10ms
                continue;
            }
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - Video render полностью под AVSYNC
        // Проверяем AVSYNC gate ПЕРЕД любым render
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            if (!avsync_gate_is_open(&ctx->avsync_gate)) {
                // ⛔ WAIT, но НЕ spin
                usleep(5000); // 5ms
                continue;
            }
        }
        
        // Шаг 34.3: Renderer starvation guard
        // 🔴 ШАГ 5: НЕТ КАДРОВ → НЕТ РЕНДЕРА → НЕТ SWAP
        if (frame_queue_size((FrameQueue *)frame_queue) == 0) {
            usleep(2000); // 2ms
            continue;
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 9.8
        // 🔥 Queue hard limit
        int queue_size = frame_queue_size((FrameQueue *)frame_queue);
        if (queue_size > VIDEO_QUEUE_MAX) {
            // Drop oldest frames
            ALOGW("⚠️ QUEUE OVERFLOW: size=%d > MAX=%d, dropping oldest", queue_size, VIDEO_QUEUE_MAX);
            while (queue_size > VIDEO_QUEUE_MAX) {
                frame_queue_drop_oldest((FrameQueue *)frame_queue);
                queue_size = frame_queue_size((FrameQueue *)frame_queue);
            }
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 9.10
        // ASSERT(queue_size <= 3)
        #ifdef DEBUG
        if (queue_size > VIDEO_QUEUE_MAX) {
            ALOGE("❌ VIDEO_QUEUE_ASSERT FAILED: queue_size=%d > MAX=%d (FATAL)", queue_size, VIDEO_QUEUE_MAX);
            abort(); // 🔥 FATAL в debug
        }
        #endif
        
        // Шаг 41.1, 41.5: Получаем текущий и следующий кадр
        Frame *f0 = frame_queue_peek_ptr((FrameQueue *)frame_queue);
        if (!f0 || !f0->frame) {
            // Нет кадра - ждём
            // 🔴 ШАГ 5: НЕТ КАДРОВ → НЕТ РЕНДЕРА → НЕТ SWAP
            usleep(2000); // 2ms
            continue;
        }
        
        // 🔎 DIAGNOSTIC: Log frame info when extracted from queue
        double f0_pts_sec = frame_pts_sec(f0->frame, vr->time_base);
        if (isnan(f0_pts_sec)) {
            f0_pts_sec = f0->pts; // Fallback на сохранённый PTS
        }
        ALOGI("🖼 VIDEO FRAME SUBMITTED TO GL: pts=%.3f size=%dx%d format=%d",
              f0_pts_sec,
              f0->frame ? f0->frame->width : 0,
              f0->frame ? f0->frame->height : 0,
              f0->frame ? f0->frame->format : -1);
        
        Frame *f1 = frame_queue_peek_next_ptr((FrameQueue *)frame_queue);
        
        // Шаг 41.5: Получаем PTS кадров в секундах (ПЕРЕД обновлением clock)
        double raw_pts0 = frame_pts_sec(f0->frame, vr->time_base);
        if (isnan(raw_pts0)) {
            raw_pts0 = f0->pts; // Fallback на сохранённый PTS
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - PATCH 2: Классификатор PTS
        // Вычисляем expected_delta: если avg_frame_rate валиден → 1/fps, иначе → 0.04 (25fps fallback)
        double expected_delta = 0.04; // 25fps fallback
        if (vs && vs->video_stream && vs->video_stream->avg_frame_rate.num > 0 && vs->video_stream->avg_frame_rate.den > 0) {
            expected_delta = (double)vs->video_stream->avg_frame_rate.den / (double)vs->video_stream->avg_frame_rate.num;
        }
        
        // Классифицируем кадр
        double last_pts = vs ? vs->last_pts : -1.0;
        FramePtsClass frame_class = classify_frame_pts(raw_pts0, last_pts, expected_delta);
        
        // Используем raw_pts0 для дальнейшей обработки (без effective_pts fallback)
        double pts0 = raw_pts0;
        
        double pts1 = NAN;
        if (f1 && f1->frame) {
            pts1 = frame_pts_sec(f1->frame, vr->time_base);
            if (isnan(pts1)) {
                pts1 = f1->pts; // Fallback
            }
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.6: SEEK + FRAME POLICY
        // Первый кадр после seek: не применяем DROP по diff, не сравниваем с audio
        // Принимаем первый валидный PTS
        // Используем has_frame вместо clock.valid (ШАГ 17)
        bool first_frame_not_rendered = !vs || !vs->has_frame;
        
        if (first_frame_not_rendered) {
            // 🔥 SAFETY-NET: render ЛЮБОЙ кадр для первого frame
            // Это обязательный фикс против: чёрного экрана, вечного waitingFirstFrame, deadlock при seek
            video_render_gl_draw(vr, f0->frame, f1 ? f1->frame : NULL, 0.0f);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.5: FIRST FRAME = VIDEO CLOCK INIT
            // Обновляем clock после eglSwapBuffers (уже выполнено в video_render_gl_draw)
            // Используем каноническую функцию video_clock_on_frame_render()
            if (vs && f0 && f0->frame) {
                extern void video_clock_on_frame_render(VideoState *vs, AVFrame *frame);
                video_clock_on_frame_render(vs, f0->frame);
            }
            
            // Обновляем master_clock_ms для обратной совместимости
            if (vs && vs->player_ctx && vs->clock.valid) {
                PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
                ctx->master_clock_ms = (int64_t)(vs->clock.pts_sec * 1000.0);
                
                // Обновляем video clock в AVSyncGate
                int64_t clock_us = (int64_t)(vs->clock.pts_sec * 1000000.0);
                avsync_gate_update_video_clock(&ctx->avsync_gate, clock_us);
            }
            
            vr->first_frame_rendered = 1;
            if (vs) {
                vs->first_frame_rendered = 1;
            }
            
            frame_queue_next((FrameQueue *)frame_queue);
            continue; // goto done
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.6: ЖЁСТКАЯ защита seek target
        // Проверяем, что кадр из правильной эпохи (serial совпадает) и pts >= seek_target
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.4: Фильтрация старых эпох
            // Если кадр из старой эпохи (serial не совпадает), дропаем его
            int current_serial = atomic_load(&ctx->seek_serial);
            if (f0->serial != current_serial) {
                ALOGW("⚠️ FRAME DROP: seek serial mismatch (drop, frame_serial=%d != current_serial=%d)", 
                      f0->serial, current_serial);
                frame_queue_next((FrameQueue *)frame_queue);
                continue;
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.6: ЖЁСТКАЯ защита seek target
            // Если seek в процессе, дропаем кадры до тех пор, пока не найдём первый >= target
            if (ctx->seek.in_progress) {
                double seek_target_sec = ctx->seek.target_ms / 1000.0;
                if (!isnan(pts0) && pts0 >= 0.0 && pts0 + 0.002 < seek_target_sec) {
                    // ❌ ещё не достигли target → drop
                    ALOGD("🔍 SEEK MODE: dropping frame pts=%.3f < target=%.3f", pts0, seek_target_sec);
                    frame_queue_next((FrameQueue *)frame_queue);
                    continue;
                }
                
                // 🔥 ПЕРВЫЙ КАДР >= target — РЕНДЕР
                video_render_gl_draw(vr, f0->frame, f1 ? f1->frame : NULL, 0.0f);
                
                // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.3
                // Обновляем clock после eglSwapBuffers (уже выполнено в video_render_gl_draw)
                // Используем каноническую функцию video_clock_on_frame_render()
                if (vs && f0 && f0->frame) {
                    extern void video_clock_on_frame_render(VideoState *vs, AVFrame *frame);
                    video_clock_on_frame_render(vs, f0->frame);
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.7: FIRST FRAME AFTER SEEK
                // Эмитим firstFrameAfterSeek ПОСЛЕ eglSwapBuffers (уже эмитится в video_render_gl_draw)
                // Завершаем seek mode
                ctx->seek.in_progress = false;
                ctx->seek_in_progress = 0;
                ctx->waiting_first_frame_after_seek = 0;
                
                // Обновляем master_clock_ms
                ctx->master_clock_ms = (int64_t)(pts0 * 1000.0);
                
                // Обновляем video clock в AVSyncGate
                int64_t clock_us = (int64_t)(pts0 * 1000000.0);
                avsync_gate_update_video_clock(&ctx->avsync_gate, clock_us);
                
                // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.9: ASSERT
                #ifdef DEBUG
                if (pts0 < seek_target_sec - 0.01) {
                    ALOGE("❌ SEEK_ASSERT FAILED: first_frame_pts=%.3f < seek_target=%.3f - 0.01 (FATAL)", 
                          pts0, seek_target_sec);
                    abort(); // 🔥 FATAL в debug
                }
                #endif
                
                // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.7: Scrub Spam Protection
                // Проверяем pending seek и выполняем его, если есть
                if (ctx->has_pending_seek) {
                    double pending_seconds = ctx->pending_seek_seconds;
                    bool pending_exact = ctx->pending_seek_exact;
                    ctx->has_pending_seek = false;
                    ctx->pending_seek_seconds = 0.0;
                    ctx->pending_seek_exact = false;
                    
                    ALOGI("🔍 SEEK: Executing pending seek to %.3f sec", pending_seconds);
                    
                    // Выполняем pending seek
                    extern int player_seek(PlayerContext *ctx, double seconds, bool exact);
                    player_seek(ctx, pending_seconds, pending_exact);
                    
                    frame_queue_next((FrameQueue *)frame_queue);
                    continue; // Продолжаем с новым seek
                }
                
                frame_queue_next((FrameQueue *)frame_queue);
                continue; // Запрещено: ждать audio clock, ждать "лучший" pts, делать drop после первого render
            }
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - используем AVSyncGate для определения master clock
        // Получаем master clock из AVSyncGate вместо старой логики
        double master_time = 0.0;
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            
            // Проверяем AVSYNC gate перед использованием master clock
            if (!avsync_gate_is_open(&ctx->avsync_gate)) {
                // ⛔ AVSYNC gate закрыт → ждём, но НЕ spin
                usleep(5000); // 5ms
                continue;
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - используем avsync.master
            if (ctx->avsync.master == CLOCK_MASTER_AUDIO && ctx->avsync.audio_healthy) {
                // Audio MASTER
                master_time = ctx->avsync.audio_clock;
            } else {
                // Video MASTER (fallback)
                master_time = ctx->avsync.video_clock;
            }
        } else {
            // Fallback на старую логику если нет PlayerContext
            master_time = get_master_clock((AudioState *)audio_state, 
                                           (VideoState *)video_state);
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - проверяем, открыт ли AVSYNC gate
        bool av_sync_enabled = false;
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            av_sync_enabled = avsync_gate_is_open(&ctx->avsync_gate);
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 9.7: Broken timestamps fallback
        // Если pts = NAN, используем fallback: last_video_pts + estimated_frame_duration
        static int fallback_used_count = 0;
        if (isnan(pts0) && vs && vs->clock.valid) {
            double frame_duration_fallback = 0.04; // 25fps fallback
            if (vs->video_stream && vs->video_stream->avg_frame_rate.num > 0 && vs->video_stream->avg_frame_rate.den > 0) {
                frame_duration_fallback = (double)vs->video_stream->avg_frame_rate.den / (double)vs->video_stream->avg_frame_rate.num;
            }
            pts0 = vs->clock.pts_sec + frame_duration_fallback;
            fallback_used_count++;
            ALOGW("⚠️ BROKEN_PTS: using fallback pts=%.3f (last=%.3f + duration=%.3f)", 
                  pts0, vs->clock.pts_sec, frame_duration_fallback);
            
            #ifdef DEBUG
            if (fallback_used_count > 1) {
                ALOGE("❌ BROKEN_PTS_ASSERT: fallback_used_count=%d > 1 (FATAL)", fallback_used_count);
                abort(); // 🔥 FATAL в debug
            }
            #endif
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 9.4: Применение should_drop_frame()
        // Получаем audio clock для drop policy
        double audio_clock_for_drop = NAN;
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            if (ctx->avsync.master == CLOCK_MASTER_AUDIO && ctx->avsync.audio_healthy) {
                audio_clock_for_drop = ctx->avsync.audio_clock;
            }
        }
        
        // Применяем drop policy
        if (should_drop_frame(vr, vs, f0, pts0, frame_class, audio_clock_for_drop, master_time)) {
            // ⚠️ НЕ swap, НЕ update clock при дропе
            frame_queue_next((FrameQueue *)frame_queue);
            continue;
        }
        
        // ✅ Кадр прошёл drop policy → рендерим
        // 🔥 PATCH 7: УДАЛЕНО "sleep until pts", "delay rendering" - кадр рендерится немедленно
        
        // 🔥 КРИТИЧНО: Определяем, есть ли аудио (для правильного выбора master clock)
        bool has_audio_active = false;
        if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            has_audio_active = (ctx->has_audio == 1) && (audio_state && clock_is_active(&((AudioState *)audio_state)->clock));
        } else {
            has_audio_active = (audio_state && clock_is_active(&((AudioState *)audio_state)->clock));
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Объявляем diff в широкой области видимости для логирования
        double diff = 0.0;  // Инициализируем для использования в логировании
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - FIRST FRAME HARDENING (анти-deadlock)
        // Первый кадр ВСЕГДА рендерится без AVSYNC
        if (!vr->first_frame_rendered) {
            // 🔒 Жёсткое правило: первый кадр рендерим немедленно, игнорируя sync
            // Применяется после: prepare, seek, surface recreate
            // Это убирает чёрный экран, предотвращает deadlock, синхронизирует pipeline
            ALOGI("🎬 FIRST_FRAME: rendering immediately (ignore sync)");
            // Продолжаем к рендеру без проверок sync
        } else if (vs && vs->player_ctx) {
            PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - PATCH 3: Seek mode = NO AVSYNC
            // Во время seek AVSYNC отключается полностью, frame drop отключён
            // Seek = find ≥ target, а не sync beauty
            if (ctx->seek.in_progress || ctx->waiting_first_frame_after_seek) {
                // Seek в процессе - рендерим первый валидный кадр без sync и без drop
                ALOGI("🔍 SEEK: rendering first valid frame @ %.3f (NO AVSYNC, NO DROP)", pts0);
                // Продолжаем к рендеру без проверок sync и без drop
            } else {
                // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.7
                // 🧠 AVSYNC (после шага 17)
                // Теперь формула чистая и детерминированная:
                // diff = video_clock - audio_clock;
                // diff > +threshold → DROP video
                // diff < -threshold → HOLD video
                // diff ≈ 0 → render
                // threshold = max(0.04, frame_duration)
                // ❌ УДАЛЕНО: frame_timer из sync
                // ❌ УДАЛЕНО: last_duration из sync
                // ❌ УДАЛЕНО: vsync time из sync
                // ❌ УДАЛЕНО: system clock из sync
                // ✅ ИСПОЛЬЗУЕМ: diff = video_clock - audio_clock (чистая формула)
                double audio_clock = ctx->avsync.audio_clock;
                double video_clock = ctx->avsync.video_clock;
                diff = video_clock - audio_clock;  // Обновляем diff для использования в логировании
                
                // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - PATCH 6: Audio master vs Video master
                // if (master == AV_MASTER_AUDIO) → video подстраивается
                // 100ms — ЖЁСТКИЙ лимит
                // если audio сильно впереди — video дропается, не ждёт
                if (ctx->avsync.master == CLOCK_MASTER_AUDIO && ctx->avsync.audio_healthy) {
                    if (!isnan(pts0) && pts0 >= 0.0 && pts0 > audio_clock + 0.100) {
                        // ❌ DROP: video опережает audio > 100ms
                        ALOGW("⚠️ FRAME DROP: video ahead of audio by %.3f sec (drop, audio master)", pts0 - audio_clock);
                        frame_queue_next((FrameQueue *)frame_queue);
                        vr->interp_stats.drop_count++;
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - anti infinite drop
                        static int consecutive_drops = 0;
                        consecutive_drops++;
                        if (consecutive_drops > 5) {
                            ALOGW("🚨 INFINITE_DROP: %d consecutive drops - hard resync", consecutive_drops);
                            extern void avsync_hard_resync(PlayerContext *ctx);
                            avsync_hard_resync(ctx);
                            consecutive_drops = 0;
                        }
                        
                        usleep(2000); // 2ms
                        continue;
                    }
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.4: HOLD RULES (МЯГКИЕ)
                // Видео отстаёт от аудио → HOLD
                // diff < -AVSYNC_THRESHOLD → HOLD_FRAME()
                // ⛔ НО: if (hold_time > MAX_FRAME_HOLD_SEC) → FORCE_RENDER()
                // Иначе deadlock на плохом аудио clock
                static double hold_start_time = 0.0;  // Глобальная static переменная для отслеживания hold
                static int hold_frame_count = 0;
                
                if (diff < -AVSYNC_THRESHOLD) {
                    // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.4: Защита от deadlock
                    // Отслеживаем время hold для защиты от бесконечного ожидания
                    double current_time = get_monotonic_time_sec();
                    
                    if (hold_start_time == 0.0) {
                        hold_start_time = current_time;
                        hold_frame_count = 0;
                    }
                    hold_frame_count++;
                    
                    double hold_duration = current_time - hold_start_time;
                    
                    // ⛔ Защита от deadlock: если hold > MAX_FRAME_HOLD_SEC → FORCE_RENDER
                    if (hold_duration > MAX_FRAME_HOLD_SEC) {
                        ALOGW("⚠️ FRAME HOLD: timeout (hold=%.3f > max=%.3f) - FORCE RENDER", 
                              hold_duration, MAX_FRAME_HOLD_SEC);
                        hold_start_time = 0.0;
                        hold_frame_count = 0;
                        // Продолжаем к рендеру (FORCE_RENDER)
                    } else {
                        // HOLD: ждём, пока video не догонит audio
                        ALOGD("⏸ FRAME HOLD: video behind audio (diff=%.3f, hold=%.3f)", 
                              diff, hold_duration);
                        usleep(5000); // 5ms
                        continue; // Пропускаем этот кадр, ждём следующего
                    }
                } else {
                    // Сбрасываем hold timer если diff в норме
                    if (hold_start_time != 0.0) {
                        hold_start_time = 0.0;
                        hold_frame_count = 0;
                    }
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - политика 1: Audio lead (video не успевает)
                // Video отстаёт от audio (drift < 0) - legacy код для больших drift
                if (diff < -0.150) {  // AV_DESYNC_WARN (150ms)
                    double abs_diff = fabs(diff);
                    
                    if (abs_diff > 0.800) {
                        // >800ms → 🔁 VIDEO RESYNC (уже обработано в avsync.c)
                        // Здесь: force render next frame >= audio_clock
                        if (!isnan(pts0) && pts0 >= audio_clock - 0.050) {
                            // Рендерим первый кадр >= audio_clock
                            ALOGI("🔁 VIDEO RESYNC: rendering frame @ %.3f (>= audio_clock %.3f)", pts0, audio_clock);
                            // Продолжаем к рендеру
                        } else {
                            // Кадр всё ещё < audio_clock → drop
                            ALOGW("⚠️ VIDEO RESYNC: dropping frame @ %.3f (< audio_clock %.3f)", pts0, audio_clock);
                            frame_queue_next((FrameQueue *)frame_queue);
                            usleep(2000);
                            continue;
                        }
                    } else if (abs_diff > 0.300) {
                        // 300-800ms → ❌❌ AGGRESSIVE DROP (без рендера)
                        ALOGW("⚠️ AVSYNC: AGGRESSIVE DROP (drift=%.3f, 300-800ms) - no render", diff);
                        frame_queue_next((FrameQueue *)frame_queue);
                        vr->interp_stats.drop_count++;
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - anti infinite drop
                        static int consecutive_drops = 0;
                        consecutive_drops++;
                        if (consecutive_drops > 5) {
                            ALOGW("🚨 INFINITE_DROP: %d consecutive drops - hard resync", consecutive_drops);
                            extern void avsync_hard_resync(PlayerContext *ctx);
                            avsync_hard_resync(ctx);
                            consecutive_drops = 0;
                        }
                        
                        usleep(2000); // 2ms
                        continue;
                    } else {
                        // 150-300ms → ❌ DROP video frames (до догоняния)
                        ALOGW("⚠️ AVSYNC: DROP frames (drift=%.3f, 150-300ms)", diff);
                        frame_queue_next((FrameQueue *)frame_queue);
                        vr->interp_stats.drop_count++;
                        
                        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - anti infinite drop
                        static int consecutive_drops = 0;
                        consecutive_drops++;
                        if (consecutive_drops > 5) {
                            ALOGW("🚨 INFINITE_DROP: %d consecutive drops - hard resync", consecutive_drops);
                            extern void avsync_hard_resync(PlayerContext *ctx);
                            avsync_hard_resync(ctx);
                            consecutive_drops = 0;
                        }
                        
                        usleep(2000); // 2ms
                        continue;
                    }
                }
                
                // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - force render после resync
                if (ctx->avsync.recovering && !isnan(pts0) && pts0 >= ctx->avsync.audio_clock - 0.050) {
                    // После resync: рендерим первый кадр >= audio_clock
                    ALOGI("🔁 VIDEO RESYNC: rendering first frame >= audio_clock @ %.3f", pts0);
                    ctx->avsync.recovering = false;
                    // Продолжаем к рендеру
                }
                
                // ✅ Normal sync (|diff| ≤ 150ms)
                // Рендерим кадр
            }
        } else {
            // Fallback: нет player context - рендерим без sync
        }
        
        // ✅ Normal sync - рендерим кадр (все проверки drop policy уже выполнены выше)
        
        // ✅ В ОКНЕ — РЕНДЕР (кадр в допустимом PTS-окне: -5ms .. +5ms)
        // 🔴 ЭТАЛОН: Обновляем video_clock ТОЛЬКО ПОСЛЕ рендера
        // ⛔ decode НЕ ТРОГАЕТ clock - только render loop
        
        // Дополнительная проверка: если f1 опоздал, дропаем f0 и переходим к f1
        // Это предотвращает interpolation между опоздавшими кадрами
        if (f1 && !isnan(pts1) && (pts1 - master_time) < -VIDEO_LATE_THRESHOLD) {
            // Логируем как WARNING только аномалии (следующий кадр слишком поздно)
            ALOGW("Next frame too late: pts1=%.3f master=%.3f (drop f0, advance to f1)", 
                  pts1, master_time);
            frame_queue_next((FrameQueue *)frame_queue);
            vr->interp_stats.drop_count++;
            // 🔴 ШАГ 5: НЕТ КАДРОВ → НЕТ РЕНДЕРА → НЕТ SWAP
            usleep(2000); // 2ms
            continue;
        }
        
        // 🔴 ШАГ 8: Обновляем has_next_frame ПЕРЕД проверкой интерполяции
        // Это критично для правильной работы AUTO-логики
        bool has_next = (f1 && f1->frame && !isnan(pts1) && pts1 > pts0);
        vr->has_next_frame = has_next;
        
        // ШАГ 6: Adaptive Interpolation Controller (умный автопилот)
        // Проверяем, должна ли быть включена interpolation
        bool should_interpolate = false;
        
        // ШАГ 6.5: Anti-flicker (гистерезис)
        if (vr->interp_stats.toggle_cooldown > 0) {
            vr->interp_stats.toggle_cooldown--;
            // Используем текущее состояние во время cooldown
            should_interpolate = vr->interpolation_enabled;
        } else {
            if (vr->interp_mode == 1) { // INTERP_FORCE_ON
                should_interpolate = has_next;
            } else if (vr->interp_mode == 2) { // INTERP_FORCE_OFF
                should_interpolate = false;
            } else { // INTERP_AUTO (ШАГ 6)
                // 🔴 ШАГ 8: Упрощённая AUTO-логика для video-only режима
                // Для video-only интерполяция ВСЕГДА полезна (24→60, 30→60)
                // Условия, когда interpolation ЗАПРЕЩЕНА
                if (vr->paused ||
                    !has_next ||
                    frame_queue_size((FrameQueue *)frame_queue) < 2) {
                    should_interpolate = false;
                } else {
                    // 🔴 ИСПРАВЛЕНИЕ: Для video-only режима интерполяция ВСЕГДА включена
                    // (если есть два кадра в очереди)
                    // Это даёт плавность 24→60, 30→60 как в AVPlayer/ExoPlayer
                    should_interpolate = true;
                }
            }
            
            // ШАГ 6.5: Устанавливаем cooldown при смене состояния
            if (should_interpolate != vr->interpolation_enabled) {
                vr->interp_stats.toggle_cooldown = 60; // INTERP_COOLDOWN_FRAMES
            }
        }
        
        // Обновляем состояние
        vr->interpolation_enabled = should_interpolate;
        
        // Шаг 41.5, 41.7: Расчёт alpha для interpolation
        float alpha = 0.0f;
        bool use_interp = false;
        AVFrame *frame1_ptr = NULL;
        
        // 🔴 ШАГ 8: КРИТИЧНО - Fallback на один кадр (ОБЯЗАТЕЛЬНО)
        // Если нет второго кадра или интерполяция отключена → рендерим только f0
        if (!should_interpolate || !has_next) {
            // Fallback: рендерим только frame0 (alpha = 0.0, frame1 = NULL)
            alpha = 0.0f;
            frame1_ptr = NULL;
            use_interp = false;
            
            // ❌ УБРАНО: Логирование fallback каждый кадр (забивает Logcat)
            // Логируем только аномалии (например, если очередь пуста слишком долго)
        } else {
            // Есть два кадра и интерполяция включена → рассчитываем alpha
            double gap = pts1 - pts0;
            
            // Шаг 41.7: Interpolation safety check
            bool interp_allowed = gap > INTERP_MIN_GAP &&
                                  gap < INTERP_MAX_GAP;
            
            if (interp_allowed) {
                // Шаг 41.5: Расчёт raw alpha
                float alpha_raw = compute_interpolation_alpha(master_time, pts0, pts1);
                
                // 🔴 ШАГ 8: Защита от NaN (КРИТИЧНО)
                if (isnan(alpha_raw) || isinf(alpha_raw)) {
                    ALOGE("❌ Alpha is NaN/Inf: alpha_raw=%.3f, pts0=%.3f pts1=%.3f master=%.3f", 
                          alpha_raw, pts0, pts1, master_time);
                    alpha = 0.0f;  // Fallback на один кадр
                    use_interp = false;
                    frame1_ptr = NULL;
                } else {
                    // ШАГ 8: Temporal smoothing для alpha (sub-pixel jitter compensation)
                    double jitter = vr->interp_stats.jitter;
                    alpha = smooth_alpha(vr, alpha_raw, jitter);
                    
                    // 🔴 Дополнительная защита от NaN после smoothing
                    if (isnan(alpha) || isinf(alpha)) {
                        ALOGE("❌ Alpha is NaN/Inf after smoothing: alpha=%.3f", alpha);
                        alpha = 0.0f;
                        use_interp = false;
                        frame1_ptr = NULL;
                    } else {
                        // Clamp alpha в [0..1] (дополнительная защита)
                        if (alpha < 0.0f) alpha = 0.0f;
                        if (alpha > 1.0f) alpha = 1.0f;
                        
                        use_interp = true;
                        frame1_ptr = f1->frame;
                        
                        // ❌ УБРАНО: Логирование интерполяции каждый кадр (забивает Logcat)
                        // Логируем только аномалии (alpha залип, gap слишком большой/маленький)
                    }
                }
            } else {
                // Gap слишком большой или маленький → без interpolation (fallback на один кадр)
                alpha = 0.0f;
                frame1_ptr = NULL;
                use_interp = false;
                // ❌ УБРАНО: Логирование gap каждый кадр (забивает Logcat)
                // Логируем только если это аномалия (например, gap постоянно слишком большой)
            }
        }
        
        // Шаг 41.6: Рендерим с interpolation (или без, если fallback)
        // 🔴 КРИТИЧНО: video_render_gl_draw всегда получает валидный alpha (0.0 если нет интерполяции)
        int ret = video_render_gl_draw(vr, f0->frame, frame1_ptr, alpha);
        
        // Шаг 41.9: Субтитры рисуются ПОСЛЕ видео (по master clock, не по video pts)
        // Субтитры НЕ интерполируются и НЕ зависят от frame0/frame1
        if (ret == 0 && vs && vs->subtitle_manager) {
            const SubtitleItem *subtitle = subtitle_manager_get_active(
                vs->subtitle_manager, 
                master_time  // Шаг 41.9: Используем master_time (audio или video clock)
            );
            if (subtitle && subtitle->text) {
                video_render_gl_subtitle(vr, subtitle->text, master_time);
            }
        }
        
        // 🔴 КРИТИЧНО: Для ImageTexture ОБЯЗАТЕЛЬНО вызываем markFrameAvailable после каждого рендеринга
        // Даже если ret != 0, нужно уведомить Flutter (для синхронизации)
        if (vr->render_target == RENDER_TARGET_IMAGE_TEXTURE && ret == 0) {
            // markFrameAvailable уже вызван внутри video_render_gl_draw() для ImageTexture
            // Но логируем для диагностики
            ALOGD("✅ ImageTexture: Frame rendered successfully, markFrameAvailable already called");
        }
        
        if (ret == 0) {
            // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE FIX - PATCH 4: update clock ТОЛЬКО после eglSwapBuffers
            // video_clock_pts обновляется внутри video_render_gl_draw() после eglSwapBuffers
            // Здесь только обновляем last_pts для frame drop policy
            
            // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 8.10
            // 🧪 ASSERT (ОБЯЗАТЕЛЬНЫ)
            // ASSERT(video_clock.pts_monotonic)
            if (vs && vs->clock.valid && !isnan(vs->clock.pts_sec) && !isnan(pts0) && pts0 >= 0.0) {
                double rendered_pts = pts0;
                if (rendered_pts + 0.001 < vs->clock.pts_sec) {
                    ALOGE("❌ VIDEO CLOCK BACKWARD: %.3f -> %.3f (FATAL)",
                          vs->clock.pts_sec,
                          rendered_pts);
                    // Используем stdlib abort() напрямую (конфликт с параметром abort функции)
                    // Используем exit() вместо abort() для избежания конфликта имен с параметром функции
                    exit(1); // 🔥 FATAL
                }
            }
            
            if (vs && !isnan(pts0) && pts0 >= 0.0) {
                // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - обновляем last_pts после рендера
                vs->last_pts = pts0;
            }
            
            // Обновляем master_clock_ms для обратной совместимости
            if (vs && vs->player_ctx && !isnan(pts0) && pts0 >= 0.0) {
                PlayerContext *ctx = (PlayerContext *)vs->player_ctx;
                ctx->master_clock_ms = (int64_t)(pts0 * 1000.0);
                
            // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO CLOCK SOURCE UNIFICATION - ШАГ 17.7
            // Обновляем avsync.video_clock из clock.pts_sec (PTS-based)
            ctx->avsync.video_clock = ctx->video && ctx->video->clock.valid ? ctx->video->clock.pts_sec : pts0;
            ctx->avsync.last_video_clock_ts = av_gettime() / 1000;  // миллисекунды
            
            // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.7: ПОЛНЫЙ АЛГОРИТМ
            // Вычисляем drift (video - audio) - чистая формула
            ctx->avsync.drift = ctx->avsync.video_clock - ctx->avsync.audio_clock;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: VIDEO FRAME DROP POLICY - ШАГ 18.9: ASSERT (ОБЯЗАТЕЛЬНЫ)
            #ifdef DEBUG
            // ASSERT(video_clock >= last_video_clock)
            static double last_video_clock = 0.0;
            if (ctx->avsync.video_clock < last_video_clock - 0.001) {
                ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock=%.3f < last=%.3f (FATAL)", 
                      ctx->avsync.video_clock, last_video_clock);
                abort(); // 🔥 FATAL в debug
            }
            last_video_clock = ctx->avsync.video_clock;
            
            // ASSERT(!isnan(video_clock))
            if (isnan(ctx->avsync.video_clock)) {
                ALOGE("❌ VIDEO_CLOCK_ASSERT FAILED: video_clock is NAN (FATAL)");
                abort(); // 🔥 FATAL в debug
            }
            
            // ASSERT(diff < 2.0)
            if (fabs(ctx->avsync.drift) > 2.0) {
                ALOGE("❌ AVSYNC_ASSERT FAILED: drift=%.3f > 2.0 (FATAL)", ctx->avsync.drift);
                abort(); // 🔥 FATAL в debug
            }
            
            // ASSERT(audio_clock monotonic)
            static double last_audio_clock = 0.0;
            if (ctx->avsync.audio_clock < last_audio_clock - 0.001) {
                ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock regression (%.3f < %.3f)", 
                      ctx->avsync.audio_clock, last_audio_clock);
                // В release не abort, только логируем
            }
            last_audio_clock = ctx->avsync.audio_clock;
            
            if (isnan(ctx->avsync.audio_clock)) {
                ALOGE("❌ AVSYNC_ASSERT FAILED: audio_clock is NAN");
            }
            #endif
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 6.8
            // 🧠 AVSYNC-GATE (открывается ОДИН РАЗ)
            // 🚫 Никаких play / decode до этого
            if (!ctx->seek.in_progress && ctx->waiting_first_frame_after_seek) {
                double seek_target_sec = (double)ctx->seek.target_ms / 1000.0;
                
                // 🔥 ШАГ 6.10: ASSERT (ОБЯЗАТЕЛЬНО)
                #ifdef DEBUG
                // ASSERT(first_frame_pts >= seek_target - 0.01)
                if (pts0 < seek_target_sec - 0.01) {
                    ALOGE("❌ SEEK ASSERT FAILED: first_frame_pts=%.3f < seek_target=%.3f (FATAL)", 
                          pts0, seek_target_sec);
                    abort(); // 🔥 FATAL в debug
                }
                
                // ASSERT(!(audio_clock > video_clock + 0.5))
                if (ctx->audio && ctx->audio->clock.valid) {
                    extern double audio_get_clock(AudioState *as);
                    double audio_clock = audio_get_clock(ctx->audio);
                    if (audio_clock > pts0 + 0.5) {
                        ALOGE("❌ SEEK ASSERT FAILED: audio_clock=%.3f > video_clock=%.3f + 0.5 (FATAL)", 
                              audio_clock, pts0);
                        // Используем exit() вместо abort() для избежания конфликта имен с параметром abort функции
                        exit(1); // 🔥 FATAL в debug
                    }
                }
                #endif
                
                ALOGI("🔍 SEEK[%ld]: first frame render @ %.3f (target=%.3f)", 
                      (long)ctx->seek.seek_id, pts0, seek_target_sec);
                
                // 🔥 ШАГ 6.8: AVSYNC-GATE открывается ОДИН РАЗ
                avsync_gate_set_seek_in_progress(&ctx->avsync_gate, false);
                avsync_gate_set_valid(&ctx->avsync_gate);  // AVSYNC ON
                
                // Эмитим firstFrameAfterSeek событие
                extern void native_player_emit_first_frame_after_seek_event(void);
                native_player_emit_first_frame_after_seek_event();
                
                // Сбрасываем флаг waiting_first_frame_after_seek
                ctx->waiting_first_frame_after_seek = 0;
                
                ALOGI("✅ SEEK: AVSYNC-GATE opened, firstFrameAfterSeek emitted");
                
                // Возобновляем audio если есть
                if (ctx->audio && ctx->has_audio) {
                    extern void audio_resume(AudioState *as);
                    audio_resume(ctx->audio);
                    
                    extern void native_player_emit_audio_state_event(const char *state);
                    native_player_emit_audio_state_event("playing");
                    
                    // Сбрасываем drop_audio флаг
                    ctx->seek.drop_audio = false;
                }
                
                // Останавливаем seek watchdog
                extern void seek_watchdog_stop(PlayerContext *ctx);
                seek_watchdog_stop(ctx);
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-IMPLEMENTATION - FIRST_FRAME ↔ AVSYNC связь
            // Обновляем video clock в AVSyncGate ПОСЛЕ eglSwapBuffers
            int64_t clock_us = (int64_t)(pts0 * 1000000.0);
            avsync_gate_update_video_clock(&ctx->avsync_gate, clock_us);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC HARDENING - обновляем master switch логику
            extern void avsync_update(PlayerContext *ctx);
            avsync_update(ctx);
            }
            
            // Успешно отрендерено
            // 🔴 ЭТАЛОН: Логируем master clock тип и timing для диагностики
            int queue_size = frame_queue_size((FrameQueue *)frame_queue);
            bool has_audio = (audio_state && ((AudioState *)audio_state)->clock.valid);
            const char *master_type = has_audio ? "audio" : "video";
            // diff уже вычислен выше в блоке AVSYNC, используем его для логирования
            bool has_anomaly = (fabs(diff) > 0.05) || (queue_size == 0) || (queue_size > 20);
            
            #ifdef VIDEO_RENDER_DEBUG
            // Debug режим: логируем раз в 60 кадров
            static int render_log_counter = 0;
            if ((render_log_counter++ % 60) == 0) {
                ALOGD("render: master=%s pts=%.3f master=%.3f diff=%.3f q=%d", 
                      master_type, pts0, master_time, diff, queue_size);
            }
            #endif
            
            // Логируем аномалии как WARNING
            if (has_anomaly) {
                ALOGW("⚠️ Render anomaly: pts0=%.3f pts1=%.3f master=%.3f diff=%.3f alpha=%.2f q=%d",
                      pts0, isnan(pts1) ? 0.0 : pts1, master_time, diff, alpha, queue_size);
            }
            
            // 🔴 ШАГ 8: video_clock уже обновлён ПЕРЕД расчётом alpha (см. выше)
            // ❌ УБРАНО: Логирование обновления clock каждый кадр (забивает Logcat)
            
            // Шаг 41.8: Обновляем статистику
            if (vr->interp_stats.frame_count > 0) {
                double interval = pts0 - vr->interp_stats.last_pts;
                if (interval > 0.0 && interval < 1.0) { // Разумный интервал
                    double delta = interval - vr->interp_stats.avg_frame_interval;
                    vr->interp_stats.jitter += fabs(delta);
                    
                    // Обновляем средний интервал
                    vr->interp_stats.avg_frame_interval =
                        (vr->interp_stats.avg_frame_interval * (vr->interp_stats.frame_count - 1) + interval) /
                        vr->interp_stats.frame_count;
                }
            } else {
                // Первый кадр - инициализируем
                vr->interp_stats.last_pts = pts0;
            }
            
            vr->interp_stats.frame_count++;
            vr->interp_stats.last_pts = pts0;
            vr->interp_stats.last_update_time = master_time;
            
            // ШАГ 6.4: Adaptive interpolation (каждые 30 кадров)
            // Логика переключения уже выполнена выше в should_interpolate
            if (vr->interp_stats.frame_count % 30 == 0) {
                // Сброс статистики для следующего периода
                vr->interp_stats.jitter = 0.0;
                vr->interp_stats.drop_count = 0;
            }
            
            // Продвигаем очередь, если alpha >= 1.0 или нет interpolation
            if (alpha >= 1.0f || !use_interp) {
                frame_queue_next((FrameQueue *)frame_queue);
            }
        } else {
            // Ошибка рендеринга
            ALOGE("Error rendering frame, dropping. PTS: %.3f", pts0);
            frame_queue_next((FrameQueue *)frame_queue);
        }
    }
    
    // 🔴 ШАГ 5: Render loop вышел из цикла (abort установлен)
    ALOGI("🛑 VSync-driven render loop stopped (abort requested)");
    
    // 🔥 КРИТИЧНО: EGLContext ОБЯЗАН быть уничтожен в render thread (где он был создан)
    // Это единственный правильный способ избежать "call to OpenGL ES API with no current context"
    // Правильная последовательность (как в VLC / ExoPlayer):
    // 1. eglMakeCurrent(NULL) - отвязываем context от текущего потока
    // 2. eglDestroySurface - уничтожаем surface
    // 3. eglDestroyContext - уничтожаем context
    // 4. eglTerminate - завершаем display
    
    ALOGI("🔧 Render thread: Cleaning up EGL resources...");
    
    // 🔥 ШАГ 1: Отвязываем context от текущего потока
    if (vr->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        ALOGI("✅ Render thread: EGL context detached");
        
        // 🔥 ШАГ 2: Уничтожаем surface
        if (vr->egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(vr->egl_display, vr->egl_surface);
            vr->egl_surface = EGL_NO_SURFACE;
            ALOGI("✅ Render thread: EGL surface destroyed");
        }
        
        // 🔥 ШАГ 3: Уничтожаем context
        if (vr->egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(vr->egl_display, vr->egl_context);
            vr->egl_context = EGL_NO_CONTEXT;
            ALOGI("✅ Render thread: EGL context destroyed");
        }
        
        // 🔥 ШАГ 4: Завершаем display
        eglTerminate(vr->egl_display);
        vr->egl_display = EGL_NO_DISPLAY;
        ALOGI("✅ Render thread: EGL display terminated");
    }
    
    vr->egl_current = false;
    ALOGI("✅ Render thread: EGL cleanup complete, thread exiting");
}

void video_render_gl_release(VideoRenderGL *vr) {
    if (!vr) {
        return;
    }
    
    // Шаг 35.9: Safe release
    pthread_mutex_lock(&vr->render_mutex);
    
    vr->state = VR_STATE_RELEASING;
    
    if (vr->initialized) {
        // 🔥 КРИТИЧНО: Проверяем, не уничтожен ли уже EGL в render thread
        // Если EGL уже уничтожен (egl_context == EGL_NO_CONTEXT), значит render thread уже завершился
        // и очистил все OpenGL ресурсы. В этом случае нам нечего делать.
        if (vr->egl_context == EGL_NO_CONTEXT || vr->egl_display == EGL_NO_DISPLAY) {
            ALOGI("✅ video_render_gl_release: EGL already destroyed in render thread, skipping OpenGL cleanup");
        } else {
            // EGL ещё жив - пытаемся очистить OpenGL ресурсы
            // Но это может не сработать, если context не current в этом потоке
            // В идеале это должно быть сделано в render thread перед его завершением
            ALOGW("⚠️ video_render_gl_release: EGL still alive, OpenGL cleanup may fail if context not current");
            
            // Пытаемся сделать context current для очистки (может не сработать)
            EGLSurface target_surface = (vr->render_target == RENDER_TARGET_IMAGE_TEXTURE) 
                ? EGL_NO_SURFACE 
                : vr->egl_surface;
            
            EGLBoolean egl_result = EGL_FALSE;
            if (target_surface != EGL_NO_SURFACE || vr->render_target == RENDER_TARGET_IMAGE_TEXTURE) {
                egl_result = eglMakeCurrent(vr->egl_display, target_surface, target_surface, vr->egl_context);
            } else {
                egl_result = eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            
            if (egl_result) {
                // Context успешно сделан current - можем очистить OpenGL ресурсы
                // 🔴 КРИТИЧНО: Освобождаем FBO для ImageTexture
                if (vr->fbo != 0) {
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    glDeleteFramebuffers(1, &vr->fbo);
                    vr->fbo = 0;
                }
                if (vr->fbo_texture != 0) {
                    glDeleteTextures(1, &vr->fbo_texture);
                    vr->fbo_texture = 0;
                }
                
                // Удаляем OpenGL ресурсы
                if (vr->tex_y) glDeleteTextures(1, &vr->tex_y);
                if (vr->tex_u) glDeleteTextures(1, &vr->tex_u);
                if (vr->tex_v) glDeleteTextures(1, &vr->tex_v);
                if (vr->vbo) glDeleteBuffers(1, &vr->vbo);
                if (vr->shader_program) glDeleteProgram(vr->shader_program);
                
                // Отвязываем context после очистки
                eglMakeCurrent(vr->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                ALOGI("✅ video_render_gl_release: OpenGL resources cleaned");
            } else {
                // Не удалось сделать context current - пропускаем очистку OpenGL ресурсов
                // Они будут очищены в render thread или уже очищены
                ALOGW("⚠️ video_render_gl_release: Failed to make EGL context current (may be destroyed in render thread), skipping OpenGL cleanup");
            }
        }
        
        // Освобождаем последний кадр (не требует GL context)
        if (vr->last_frame) {
            av_frame_free(&vr->last_frame);
        }
        
        // 🔥 КРИТИЧНО: EGL ресурсы НЕ уничтожаем здесь (в JNI потоке)
        // EGLContext ОБЯЗАН быть уничтожен в render thread (где он был создан)
        // Это делается в конце video_render_gl_render_loop() перед выходом из потока
        // 
        // ❌ НЕ ДЕЛАЕМ:
        // - eglDestroySurface
        // - eglDestroyContext
        // - eglTerminate
        //
        // ✅ ДЕЛАЕМ:
        // - Только очистка OpenGL ресурсов (если context доступен)
        // - EGL уничтожение - в render thread
    }
    
    // ШАГ 11.1: Сброс persistent textures
    vr->textures_initialized = false;
    vr->tex_w = 0;
    vr->tex_h = 0;
    vr->egl_current = false;
    
    vr->state = VR_STATE_UNINITIALIZED;
    vr->initialized = false;
    
    pthread_mutex_unlock(&vr->render_mutex);
    pthread_mutex_destroy(&vr->render_mutex);
    
    // Очищаем JNI callback
    native_player_cleanup();
    
    memset(vr, 0, sizeof(VideoRenderGL));
    ALOGI("OpenGL video renderer released");
}

/// Вычислить transform matrix для масштабирования (Resize/Rotation)
static void compute_transform(VideoRenderGL *vr, float *out_mat4) {
    float vw = vr->layout.video_w;
    float vh = vr->layout.video_h;
    float sw = vr->layout.view_w;
    float sh = vr->layout.view_h;
    
    // Если viewport не установлен - используем identity matrix
    if (sw <= 0.0f || sh <= 0.0f) {
        memset(out_mat4, 0, sizeof(float) * 16);
        out_mat4[0] = 1.0f;
        out_mat4[5] = 1.0f;
        out_mat4[10] = 1.0f;
        out_mat4[15] = 1.0f;
        return;
    }
    
    float sx = 1.0f;
    float sy = 1.0f;
    
    float video_aspect = vw / vh;
    float view_aspect = sw / sh;
    
    // Вычисляем scale в зависимости от режима
    if (vr->fit_mode == FIT_CONTAIN) { // SCALE_FIT (contain)
        if (view_aspect > video_aspect) {
            sx = video_aspect / view_aspect;
        } else {
            sy = view_aspect / video_aspect;
        }
    } else if (vr->fit_mode == FIT_COVER) { // SCALE_FILL (cover)
        if (view_aspect > video_aspect) {
            sy = view_aspect / video_aspect;
        } else {
            sx = video_aspect / view_aspect;
        }
    }
    // SCALE_STRETCH: sx = sy = 1.0f (уже установлено)
    
    // Создаём identity matrix с scale
    memset(out_mat4, 0, sizeof(float) * 16);
    out_mat4[0] = sx;
    out_mat4[5] = sy;
    out_mat4[10] = 1.0f;
    out_mat4[15] = 1.0f;
}

/// Установить viewport и параметры отображения (Resize / Rotation)
void video_render_gl_set_viewport(VideoRenderGL *vr,
                                   float view_w,
                                   float view_h,
                                   int rotation,
                                   int scale_mode) {
    if (!vr) {
        ALOGE("video_render_gl_set_viewport: vr is NULL");
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    vr->layout.view_w = view_w;
    vr->layout.view_h = view_h;
    vr->layout.rotation = rotation;
    vr->fit_mode = scale_mode; // Используем fit_mode вместо scale_mode
    
    // Обновляем viewport размеры для обратной совместимости
    vr->viewport_w = (int)view_w;
    vr->viewport_h = (int)view_h;
    
    // Обновляем video размеры, если они изменились
    if (vr->layout.video_w <= 0.0f || vr->layout.video_h <= 0.0f) {
        vr->layout.video_w = (float)vr->video_width;
        vr->layout.video_h = (float)vr->video_height;
    }
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    // 🔴 ЭТАЛОН: Пересчитываем aspect ratio при изменении viewport
    video_render_gl_update_aspect(vr);
    
    ALOGD("✅ Viewport set: view=%fx%f, video=%fx%f, rotation=%d, scaleMode=%d",
          view_w, view_h, vr->layout.video_w, vr->layout.video_h, rotation, scale_mode);
}

/// Установить transform для жестов (pinch-to-zoom, pan)
void video_render_gl_set_transform(VideoRenderGL *vr,
                                    float scale_delta,
                                    float dx,
                                    float dy) {
    if (!vr) {
        ALOGE("video_render_gl_set_transform: vr is NULL");
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    // Применяем scale delta (мультипликативно)
    vr->transform.scale *= scale_delta;
    
    // Применяем offset (аддитивно)
    vr->transform.offset_x += dx;
    vr->transform.offset_y += dy;
    
    // Clamp scale (1.0 - 4.0)
    if (vr->transform.scale < 1.0f) {
        vr->transform.scale = 1.0f;
    }
    if (vr->transform.scale > 4.0f) {
        vr->transform.scale = 4.0f;
    }
    
    // Ограничение pan (чтобы не утащить видео за пределы экрана)
    float limit = vr->transform.scale - 1.0f;
    if (limit > 0.0f) {
        if (vr->transform.offset_x > limit) {
            vr->transform.offset_x = limit;
        }
        if (vr->transform.offset_x < -limit) {
            vr->transform.offset_x = -limit;
        }
        if (vr->transform.offset_y > limit) {
            vr->transform.offset_y = limit;
        }
        if (vr->transform.offset_y < -limit) {
            vr->transform.offset_y = -limit;
        }
    } else {
        // Если scale == 1.0, сбрасываем offset
        vr->transform.offset_x = 0.0f;
        vr->transform.offset_y = 0.0f;
    }
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGD("✅ Transform set: scale=%.2f, offset=(%.3f, %.3f)",
          vr->transform.scale, vr->transform.offset_x, vr->transform.offset_y);
}

/// Сбросить transform жестов (double-tap zoom reset)
void video_render_gl_reset_transform(VideoRenderGL *vr) {
    if (!vr) {
        ALOGE("video_render_gl_reset_transform: vr is NULL");
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    vr->transform.scale = 1.0f;
    vr->transform.offset_x = 0.0f;
    vr->transform.offset_y = 0.0f;
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGD("✅ Transform reset");
}

/// Установить safe-area для субтитров
void video_render_gl_set_subtitle_safe_area(VideoRenderGL *vr,
                                             float safe_top,
                                             float safe_bottom,
                                             float safe_left,
                                             float safe_right,
                                             bool is_hdr) {
    if (!vr) {
        ALOGE("video_render_gl_set_subtitle_safe_area: vr is NULL");
        return;
    }
    
    pthread_mutex_lock(&vr->render_mutex);
    
    vr->subtitle_safe.safe_top = safe_top;
    vr->subtitle_safe.safe_bottom = safe_bottom;
    vr->subtitle_safe.safe_left = safe_left;
    vr->subtitle_safe.safe_right = safe_right;
    vr->subtitle_safe.is_hdr = is_hdr;
    
    pthread_mutex_unlock(&vr->render_mutex);
    
    ALOGD("✅ Subtitle safe-area set: top=%.1f, bottom=%.1f, left=%.1f, right=%.1f, HDR=%s",
          safe_top, safe_bottom, safe_left, safe_right, is_hdr ? "yes" : "no");
}

bool video_render_gl_is_initialized(VideoRenderGL *vr) {
    return vr && vr->initialized;
}

/// 🔴 ШАГ 3: Flutter вызывает acquireLatestImage() - возвращаем GL texture
///
/// Вызывается из Flutter Engine когда нужен новый кадр
/// Возвращает GL texture ID из flutter_buffers[read_index]
bool video_render_gl_acquire_latest_image(VideoRenderGL *vr, GLuint *texture_id_out, int *width_out, int *height_out) {
    if (!vr || !texture_id_out || !width_out || !height_out) {
        return false;
    }
    
    // 🔴 КРИТИЧНО: Только для ImageTexture mode
    if (vr->render_target != RENDER_TARGET_IMAGE_TEXTURE) {
        return false;
    }
    
    // Проверяем, что FBO создан
    if (vr->fbo == 0 || vr->fbo_texture == 0) {
        return false;
    }
    
    pthread_mutex_lock(&vr->flutter_buffer_mutex);
    
    // Возвращаем texture из read buffer
    FlutterImageBuffer *read_buffer = &vr->flutter_buffers[vr->flutter_read_index];
    
    if (read_buffer->tex_id == 0) {
        pthread_mutex_unlock(&vr->flutter_buffer_mutex);
        return false;
    }
    
    *texture_id_out = read_buffer->tex_id;
    *width_out = read_buffer->width;
    *height_out = read_buffer->height;
    
    pthread_mutex_unlock(&vr->flutter_buffer_mutex);
    
    ALOGD("🎨 ImageTexture: acquireLatestImage returned texture=%u, size=%dx%d", 
          *texture_id_out, *width_out, *height_out);
    
    return true;
}

// 🔥 КРИТИЧЕСКИЙ FIX: VSYNC_DROP_DETECT - функции-геттеры для получения счетчиков
int64_t video_render_get_swap_count(void) {
    return g_swap_count;
}

double video_render_get_first_swap_time(void) {
    return g_first_swap_time;
}

int64_t video_render_get_last_swap_ts_ms(void) {
    return g_last_swap_ts_ms;
}

// 🔥 КРИТИЧЕСКИЙ FIX: POWER_SAVE/APS_ASSERT - функция-геттер для получения FPS
int video_render_get_fps(void) {
    return g_last_fps;
}


