#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include "ffmpeg_player.h"
#include "native_player_jni.h"

#define LOG_TAG "SmartFFmpeg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Глобальные ссылки для callbacks
static JavaVM *g_jvm = NULL;
static jobject g_player_obj = NULL;
static jclass g_player_class = NULL;

// Method IDs для callbacks
static jmethodID g_on_prepared_method = NULL;
static jmethodID g_on_surface_ready_method = NULL;
static jmethodID g_on_first_frame_method = NULL;
static jmethodID g_on_first_frame_after_seek_method = NULL;
static jmethodID g_on_position_method = NULL;
static jmethodID g_on_ended_method = NULL;
static jmethodID g_on_error_method = NULL;
static jmethodID g_on_audio_state_method = NULL;

// Инициализация JNI
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    LOGI("JNI_OnLoad: SmartFFmpeg native library loaded");
    return JNI_VERSION_1_6;
}

// Получить JNIEnv для текущего потока
static JNIEnv* get_jni_env() {
    JNIEnv *env = NULL;
    if (g_jvm == NULL) {
        LOGE("get_jni_env: JavaVM is NULL");
        return NULL;
    }
    
    int status = (*g_jvm)->GetEnv(g_jvm, (void**)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
        status = (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        if (status < 0) {
            LOGE("get_jni_env: Failed to attach thread");
            return NULL;
        }
    }
    return env;
}

// Callbacks для native кода
void native_player_emit_prepared_event_with_data(PlayerContext *ctx, int has_audio, int64_t duration_ms) {
    JNIEnv *env = get_jni_env();
    if (env == NULL || g_player_obj == NULL || g_on_prepared_method == NULL) {
        LOGE("emit_prepared: Invalid JNI state");
        return;
    }
    
    (*env)->CallVoidMethod(env, g_player_obj, g_on_prepared_method, 
                          (jboolean)has_audio, (jlong)duration_ms);
}

void native_player_emit_surface_ready_event(void) {
    JNIEnv *env = get_jni_env();
    if (env == NULL || g_player_obj == NULL || g_on_surface_ready_method == NULL) {
        return;
    }
    
    (*env)->CallVoidMethod(env, g_player_obj, g_on_surface_ready_method);
}

void native_player_emit_first_frame_event(void) {
    JNIEnv *env = get_jni_env();
    if (env == NULL || g_player_obj == NULL || g_on_first_frame_method == NULL) {
        return;
    }
    
    (*env)->CallVoidMethod(env, g_player_obj, g_on_first_frame_method);
}

void native_player_emit_first_frame_after_seek_event(void) {
    JNIEnv *env = get_jni_env();
    if (env == NULL || g_player_obj == NULL || g_on_first_frame_after_seek_method == NULL) {
        return;
    }
    
    (*env)->CallVoidMethod(env, g_player_obj, g_on_first_frame_after_seek_method);
}

void native_player_emit_error_event(const char *message) {
    JNIEnv *env = get_jni_env();
    if (env == NULL || g_player_obj == NULL || g_on_error_method == NULL) {
        return;
    }
    
    jstring jmsg = (*env)->NewStringUTF(env, message);
    (*env)->CallVoidMethod(env, g_player_obj, g_on_error_method, jmsg);
    (*env)->DeleteLocalRef(env, jmsg);
}

void native_player_emit_audio_state_event(const char *state) {
    JNIEnv *env = get_jni_env();
    if (env == NULL || g_player_obj == NULL || g_on_audio_state_method == NULL) {
        return;
    }
    
    jstring jstate = (*env)->NewStringUTF(env, state);
    (*env)->CallVoidMethod(env, g_player_obj, g_on_audio_state_method, jstate);
    (*env)->DeleteLocalRef(env, jstate);
}

// JNI методы
JNIEXPORT jlong JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativePrepare(JNIEnv *env, jobject thiz, jstring path) {
    const char *c_path = (*env)->GetStringUTFChars(env, path, NULL);
    
    // Создаём контекст плеера
    PlayerContext *ctx = (PlayerContext*)calloc(1, sizeof(PlayerContext));
    if (ctx == NULL) {
        LOGE("nativePrepare: Failed to allocate PlayerContext");
        (*env)->ReleaseStringUTFChars(env, path, c_path);
        return 0;
    }
    
    // Сохраняем глобальные ссылки для callbacks
    if (g_player_obj != NULL) {
        (*env)->DeleteGlobalRef(env, g_player_obj);
    }
    g_player_obj = (*env)->NewGlobalRef(env, thiz);
    
    // Получаем method IDs
    if (g_player_class == NULL) {
        jclass local_class = (*env)->GetObjectClass(env, thiz);
        g_player_class = (*env)->NewGlobalRef(env, local_class);
        (*env)->DeleteLocalRef(env, local_class);
        
        g_on_prepared_method = (*env)->GetMethodID(env, g_player_class, 
                                                   "onPreparedCallback", "(ZJ)V");
        g_on_surface_ready_method = (*env)->GetMethodID(env, g_player_class, 
                                                        "onSurfaceReadyCallback", "()V");
        g_on_first_frame_method = (*env)->GetMethodID(env, g_player_class, 
                                                      "onFirstFrameCallback", "()V");
        g_on_first_frame_after_seek_method = (*env)->GetMethodID(env, g_player_class, 
                                                                  "onFirstFrameAfterSeekCallback", "()V");
        g_on_position_method = (*env)->GetMethodID(env, g_player_class, 
                                                   "onPositionCallback", "(J)V");
        g_on_ended_method = (*env)->GetMethodID(env, g_player_class, 
                                                "onEndedCallback", "()V");
        g_on_error_method = (*env)->GetMethodID(env, g_player_class, 
                                                "onErrorCallback", "(Ljava/lang/String;)V");
        g_on_audio_state_method = (*env)->GetMethodID(env, g_player_class, 
                                                      "onAudioStateChangedCallback", "(Ljava/lang/String;)V");
    }
    
    // Открываем медиафайл
    int ret = open_media(ctx, c_path);
    (*env)->ReleaseStringUTFChars(env, path, c_path);
    
    if (ret < 0) {
        LOGE("nativePrepare: Failed to open media");
        free(ctx);
        return 0;
    }
    
    LOGI("nativePrepare: Success, handle=%p", ctx);
    return (jlong)ctx;
}

JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeSetSurface(JNIEnv *env, jobject thiz, 
                                                               jlong handle, jobject surface) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativeSetSurface: Invalid handle");
        return;
    }
    
    ANativeWindow *window = NULL;
    if (surface != NULL) {
        window = ANativeWindow_fromSurface(env, surface);
    }
    
    // Здесь нужно вызвать функцию из video_renderer для установки surface
    // Это зависит от вашей реализации video_renderer
    LOGI("nativeSetSurface: window=%p", window);
}

JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativePlay(JNIEnv *env, jobject thiz, jlong handle) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativePlay: Invalid handle");
        return;
    }
    
    play(ctx);
    LOGI("nativePlay: Started");
}

JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativePause(JNIEnv *env, jobject thiz, jlong handle) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativePause: Invalid handle");
        return;
    }
    
    player_pause(ctx);
    LOGI("nativePause: Paused");
}

JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeSeek(JNIEnv *env, jobject thiz, 
                                                        jlong handle, jdouble seconds, jboolean exact) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativeSeek: Invalid handle");
        return;
    }
    
    player_seek(ctx, seconds, exact);
    LOGI("nativeSeek: seconds=%.2f, exact=%d", seconds, exact);
}

JNIEXPORT jlong JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeGetPosition(JNIEnv *env, jobject thiz, jlong handle) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        return 0;
    }
    
    return get_position(ctx);
}

JNIEXPORT jlong JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeGetDuration(JNIEnv *env, jobject thiz, jlong handle) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        return 0;
    }
    
    return get_duration(ctx);
}

JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeSetSpeed(JNIEnv *env, jobject thiz, 
                                                            jlong handle, jdouble speed) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativeSetSpeed: Invalid handle");
        return;
    }
    
    player_set_speed(ctx, speed);
    LOGI("nativeSetSpeed: speed=%.2f", speed);
}

JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeRelease(JNIEnv *env, jobject thiz, jlong handle) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativeRelease: Invalid handle");
        return;
    }
    
    close_media(ctx);
    free(ctx);
    
    // Очищаем глобальные ссылки
    if (g_player_obj != NULL) {
        (*env)->DeleteGlobalRef(env, g_player_obj);
        g_player_obj = NULL;
    }
    
    LOGI("nativeRelease: Released");
}
