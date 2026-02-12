/// Шаг 37: Zero-copy Video Rendering (AHardwareBuffer / OES)
///
/// Использует AHardwareBuffer для zero-copy рендеринга FFmpeg frames
/// Требует Android API 26+ и поддержки EGL_ANDROID_get_native_client_buffer

#ifndef VIDEO_RENDER_HARDWARE_H
#define VIDEO_RENDER_HARDWARE_H

#include <jni.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "libavutil/frame.h"
#include <stdbool.h>

// Forward declaration
typedef struct VideoRenderGL VideoRenderGL;

/// Zero-copy видеорендер через AHardwareBuffer
///
/// Конвертирует FFmpeg AVFrame в AHardwareBuffer и рендерит через EGLImage
/// Используется только для H.264/H.265 на Android API 26+
typedef struct VideoRenderHardware {
    /// EGL display
    EGLDisplay egl_display;
    
    /// EGL context
    EGLContext egl_context;
    
    /// AHardwareBuffer для YUV данных
    void *hardware_buffer; // AHardwareBuffer*
    
    /// EGLImage из AHardwareBuffer
    EGLImageKHR egl_image;
    
    /// GL texture (GL_TEXTURE_EXTERNAL_OES)
    GLuint texture_oes;
    
    /// Shader program для OES texture
    GLuint shader_program;
    
    /// Ширина видео
    int width;
    
    /// Высота видео
    int height;
    
    /// Флаг инициализации
    bool initialized;
    
    /// Android API level (для проверки поддержки)
    int api_level;
} VideoRenderHardware;

/// Проверить, поддерживается ли zero-copy (Шаг 37.2)
///
/// @return true если поддерживается (API 26+, H.264/H.265)
bool video_render_hardware_is_supported(void);

/// Инициализировать hardware renderer
///
/// @param vrh Hardware renderer
/// @param egl_display EGL display
/// @param egl_context EGL context
/// @param width Ширина видео
/// @param height Высота видео
/// @return 0 при успехе, <0 при ошибке
int video_render_hardware_init(VideoRenderHardware *vrh,
                                EGLDisplay egl_display,
                                EGLContext egl_context,
                                int width,
                                int height);

/// Рендерить AVFrame через AHardwareBuffer (zero-copy)
///
/// @param vrh Hardware renderer
/// @param frame AVFrame (YUV420P)
/// @return 0 при успехе, <0 при ошибке
int video_render_hardware_frame(VideoRenderHardware *vrh, AVFrame *frame);

/// Получить OES texture для использования в shader
///
/// @param vrh Hardware renderer
/// @return GL texture ID (GL_TEXTURE_EXTERNAL_OES)
GLuint video_render_hardware_get_texture(VideoRenderHardware *vrh);

/// Освободить ресурсы hardware renderer
///
/// @param vrh Hardware renderer
void video_render_hardware_release(VideoRenderHardware *vrh);

#endif // VIDEO_RENDER_HARDWARE_H

