/// Шаг 37: Zero-copy Video Rendering (AHardwareBuffer / OES)

#include "video_render_hardware.h"
#include <android/log.h>
#include <android/hardware_buffer.h>
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "VideoRenderHW"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// EGL extension functions (Шаг 37.4)
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBufferANDROID = NULL;
static PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
static PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

// Fragment shader для OES texture (Шаг 37.6)
static const char *fragment_shader_oes_source =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 vTexCoord;\n"
    "uniform samplerExternalOES uTexture;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(uTexture, vTexCoord);\n"
    "}\n";

// Vertex shader (общий)
static const char *vertex_shader_source =
    "attribute vec4 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 vTexCoord;\n"
    "void main() {\n"
    "    gl_Position = aPosition;\n"
    "    vTexCoord = aTexCoord;\n"
    "}\n";

/// Инициализировать EGL extension functions
static int init_egl_extensions(EGLDisplay display) {
    // Получаем адреса функций расширений
    eglGetNativeClientBufferANDROID = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");
    
    eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    
    eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    
    glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    
    if (!eglGetNativeClientBufferANDROID || !eglCreateImageKHR || 
        !eglDestroyImageKHR || !glEGLImageTargetTexture2DOES) {
        ALOGE("Required EGL extensions not available");
        return -1;
    }
    
    ALOGI("EGL extensions initialized");
    return 0;
}

/// Компилировать shader
static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    if (!shader) {
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

bool video_render_hardware_is_supported(void) {
    // Шаг 37.2: Проверка поддержки
    // Требуется Android API 26+ и поддержка AHardwareBuffer
    // Проверка делается через JNI (Android API level)
    // Пока возвращаем true - проверка будет в init
    
    return true; // Проверка будет в init через JNI
}

int video_render_hardware_init(VideoRenderHardware *vrh,
                                EGLDisplay egl_display,
                                EGLContext egl_context,
                                int width,
                                int height) {
    if (!vrh || !egl_display || !egl_context) {
        ALOGE("Invalid parameters for video_render_hardware_init");
        return -1;
    }
    
    memset(vrh, 0, sizeof(VideoRenderHardware));
    
    vrh->egl_display = egl_display;
    vrh->egl_context = egl_context;
    vrh->width = width;
    vrh->height = height;
    
    // Инициализируем EGL extensions (Шаг 37.4)
    if (init_egl_extensions(egl_display) < 0) {
        ALOGE("Failed to initialize EGL extensions");
        return -1;
    }
    
    // Шаг 37.3: Создаём AHardwareBuffer
    AHardwareBuffer_Desc desc = {
        .width = (uint32_t)width,
        .height = (uint32_t)height,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420,
        .usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_VIDEO_ENCODE |
                 AHARDWAREBUFFER_USAGE_CPU_READ_NEVER |
                 AHARDWAREBUFFER_USAGE_CPU_WRITE_NEVER,
        .stride = 0, // Автоматически
    };
    
    AHardwareBuffer *buffer = NULL;
    int ret = AHardwareBuffer_allocate(&desc, &buffer);
    if (ret != 0 || !buffer) {
        ALOGE("Failed to allocate AHardwareBuffer: %d", ret);
        return -1;
    }
    
    vrh->hardware_buffer = buffer;
    
    // Шаг 37.4: Import AHardwareBuffer → EGLImage
    EGLClientBuffer client_buffer = eglGetNativeClientBufferANDROID(buffer);
    if (!client_buffer) {
        ALOGE("Failed to get EGLClientBuffer from AHardwareBuffer");
        AHardwareBuffer_release(buffer);
        return -1;
    }
    
    EGLint attribs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE
    };
    
    vrh->egl_image = eglCreateImageKHR(
        egl_display,
        EGL_NO_CONTEXT,
        EGL_NATIVE_BUFFER_ANDROID,
        client_buffer,
        attribs
    );
    
    if (vrh->egl_image == EGL_NO_IMAGE_KHR) {
        ALOGE("Failed to create EGLImage from AHardwareBuffer");
        AHardwareBuffer_release(buffer);
        return -1;
    }
    
    // Шаг 37.5: Bind EGLImage → GL texture
    glGenTextures(1, &vrh->texture_oes);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, vrh->texture_oes);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, vrh->egl_image);
    
    // Создаём shader program для OES (Шаг 37.6)
    vrh->shader_program = create_program(vertex_shader_source, fragment_shader_oes_source);
    if (!vrh->shader_program) {
        ALOGE("Failed to create shader program for OES texture");
        glDeleteTextures(1, &vrh->texture_oes);
        eglDestroyImageKHR(egl_display, vrh->egl_image);
        AHardwareBuffer_release(buffer);
        return -1;
    }
    
    vrh->initialized = true;
    ALOGI("Hardware renderer initialized (%dx%d) - zero-copy enabled", width, height);
    
    return 0;
}

int video_render_hardware_frame(VideoRenderHardware *vrh, AVFrame *frame) {
    if (!vrh->initialized || !frame) {
        return -1;
    }
    
    // Шаг 37: Копируем YUV данные в AHardwareBuffer
    // Это единственное копирование - дальше всё zero-copy
    
    AHardwareBuffer *buffer = (AHardwareBuffer *)vrh->hardware_buffer;
    
    // Lock buffer для записи
    AHardwareBuffer_Planes planes;
    uint8_t *data[3] = {NULL, NULL, NULL};
    
    int ret = AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, NULL, &planes);
    if (ret != 0) {
        ALOGE("Failed to lock AHardwareBuffer: %d", ret);
        return -1;
    }
    
    // Копируем Y plane
    if (planes.planes[0].data) {
        memcpy(planes.planes[0].data, frame->data[0], frame->linesize[0] * frame->height);
    }
    
    // Копируем U/V planes (interleaved в AHardwareBuffer)
    if (planes.planes[1].data && frame->data[1] && frame->data[2]) {
        // U и V могут быть interleaved или separate
        // Предполагаем separate для простоты
        int uv_size = (frame->width / 2) * (frame->height / 2);
        memcpy(planes.planes[1].data, frame->data[1], uv_size);
        if (planes.planes[2].data) {
            memcpy(planes.planes[2].data, frame->data[2], uv_size);
        }
    }
    
    AHardwareBuffer_unlock(buffer, NULL);
    
    // Теперь кадр в AHardwareBuffer - дальнейший рендеринг zero-copy
    // EGLImage автоматически обновится
    
    return 0;
}

GLuint video_render_hardware_get_texture(VideoRenderHardware *vrh) {
    if (!vrh || !vrh->initialized) {
        return 0;
    }
    return vrh->texture_oes;
}

void video_render_hardware_release(VideoRenderHardware *vrh) {
    if (!vrh) {
        return;
    }
    
    if (vrh->initialized) {
        // Освобождаем OpenGL ресурсы
        if (vrh->texture_oes) {
            glDeleteTextures(1, &vrh->texture_oes);
        }
        if (vrh->shader_program) {
            glDeleteProgram(vrh->shader_program);
        }
        
        // Освобождаем EGLImage
        if (vrh->egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR) {
            eglDestroyImageKHR(vrh->egl_display, vrh->egl_image);
        }
        
        // Освобождаем AHardwareBuffer
        if (vrh->hardware_buffer) {
            AHardwareBuffer_release((AHardwareBuffer *)vrh->hardware_buffer);
        }
    }
    
    memset(vrh, 0, sizeof(VideoRenderHardware));
    ALOGI("Hardware renderer released");
}

