#include <jni.h>
#include <pthread.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>  // Для ANativeWindow_fromSurface
#include "native_player_jni.h"
#include "ffmpeg_player.h"
#include "ffmpeg_player_lifecycle.h"
#include "video_render_gl.h"
#include "subtitle_manager.h"
#include "native_preview.h"

#define LOG_TAG "NativePlayerJNI"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Глобальные переменные для JNI
// 🔴 ВАЖНО: g_jvm и g_renderer НЕ static, так как используются в других файлах через extern
JavaVM *g_jvm = NULL;
VideoRenderGL *g_renderer = NULL;  // VideoRenderGL instance (используется в ffmpeg_player.c и video_renderer.c)
PlayerContext *g_player_context = NULL;  // 🔥 КРИТИЧЕСКИЙ FIX: Глобальная ссылка на PlayerContext для ASSERT-ов

// 🔥 КРИТИЧЕСКИЙ FIX: SURFACE_REPLACED ASSERT - глобальная ссылка на текущий ANativeWindow
static ANativeWindow *g_current_window = NULL;

// Остальные переменные остаются static, так как используются только в этом файле
static pthread_mutex_t g_jni_mutex = PTHREAD_MUTEX_INITIALIZER;
static jobject g_event_callback = NULL;  // GlobalRef на callback объект
static jmethodID g_on_event_method = NULL;  // MethodID для onEvent callback
static jobject g_plugin = NULL;  // GlobalRef на plugin instance
static jmethodID g_onFrameAvailable = NULL;  // MethodID для onFrameAvailable

// 🔒 Native Event Contract: флаги для событий (эмитятся строго один раз)
static int g_duration_emitted = 0;
static int g_first_frame_emitted = 0;  // 🔥 FIX: firstFrame эмитится строго один раз
static int g_play_started_emitted = 0;  // 🔥 FIX: playStarted эмитится строго один раз (для диагностики)

// 🔥 КРИТИЧЕСКИЙ FIX: NEXT VIDEO - флаг для предотвращения дубликатов completed
// Используется в native_player_emit_completed_event() и сбрасывается в nativeCreatePlayerContext
static int g_completed_emitted = 0;

// 🔥 КРИТИЧЕСКИЙ FIX: Буфер событий ДО регистрации EventChannel (onListen)
// Это гарантирует, что prepared и duration не потеряются, если они пришли до onListen
static int g_event_listener_ready = 0;  // Флаг, что EventChannel подписан (onListen произошёл)
static int g_prepared_pending = 0;  // Флаг, что prepared событие ожидает отправки
static int g_prepared_has_audio = 0;  // Значение has_audio для pending prepared
static int64_t g_prepared_duration_ms = -1;  // Значение duration для pending prepared
static int64_t g_duration_pending_ms = -1;  // Значение duration для pending duration event

// 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - глобальные флаги для атомарного dispose
// prepare() запрещён, пока dispose не завершён на 100% (после join всех threads)
static int g_dispose_in_progress = 0;  // Флаг, что dispose выполняется
static int g_disposed = 1;  // Флаг, что dispose завершён (1 = disposed, 0 = active)

/// 🔒 FIX Z7: playStarted НЕ участвует в контракте событий (informational only)
/// playStarted — служебное событие для диагностики, не имеет значения для FSM
/// FSM не зависит от render / decode / threads
/// playback started определяется через position > 0, не через playStarted
/// Эмитится только для совместимости, можно удалить в будущем
void native_player_emit_play_started_event(void) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit playStarted event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: playStarted event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    // 🔥 FIX: playStarted эмитится строго один раз (для диагностики)
    if (g_play_started_emitted) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ Duplicate 'playStarted' event ignored (already emitted)");
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit playStarted event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit playStarted event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "playStarted");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём пустой payload (или NULL)
    jobject payload_map = NULL;
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in playStarted event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            // 🔥 FIX: Устанавливаем флаг ПОСЛЕ успешной отправки
            g_play_started_emitted = 1;
            ALOGI("✅ PlayStarted event emitted");
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔥 КРИТИЧЕСКИЙ FIX: PLAY_ASSERT - эмитим playAccepted когда play() реально принят native-стороной
/// Это гарантирует, что play() был вызван и прошёл AVSYNC-GATE
void native_player_emit_play_accepted_event(void) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit playAccepted event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: playAccepted event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit playAccepted event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit playAccepted event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "playAccepted");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, NULL);
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    
    ALOGI("✅ playAccepted event emitted");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: DECODE_STARTED_ASSERT - эмитим decodeStarted когда decode thread реально стартовал
/// Это гарантирует, что demux/decode threads реально запущены
void native_player_emit_decode_started_event(void) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit decodeStarted event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: decodeStarted event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit decodeStarted event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit decodeStarted event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "decodeStarted");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, NULL);
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    
    ALOGI("✅ decodeStarted event emitted");
}

// 🔥 КРИТИЧЕСКИЙ FIX: EGL_CONTEXT_LOST ASSERT - эмит события потери EGL контекста
void native_player_emit_egl_context_lost_event(void) {
    if (!g_jvm) return;
    pthread_mutex_lock(&g_jni_mutex);
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    if (!g_event_callback || !g_on_event_method) {
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    pthread_mutex_unlock(&g_jni_mutex);
    JNIEnv *env = NULL;
    int need_detach = 0;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        need_detach = 1;
    }
    jstring event = (*env)->NewStringUTF(env, "eglContextLost");
    (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event, NULL);
    (*env)->DeleteLocalRef(env, event);
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    ALOGI("✅ eglContextLost event emitted");
}

// 🔥 КРИТИЧЕСКИЙ FIX: SURFACE_REPLACED ASSERT - эмит события замены Surface
void native_player_emit_surface_replaced_event(void) {
    if (!g_jvm) return;
    pthread_mutex_lock(&g_jni_mutex);
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    if (!g_event_callback || !g_on_event_method) {
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    pthread_mutex_unlock(&g_jni_mutex);
    JNIEnv *env = NULL;
    int need_detach = 0;
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL);
        need_detach = 1;
    }
    jstring event = (*env)->NewStringUTF(env, "surfaceReplaced");
    (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event, NULL);
    (*env)->DeleteLocalRef(env, event);
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    ALOGI("✅ surfaceReplaced event emitted");
}

// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-MASTER - эмит события ошибки (FATAL условия)
void native_player_emit_error_event(const char *message) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit error event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: error event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit error event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit error event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "error");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём payload с сообщением об ошибке
    jclass map_class = (*env)->FindClass(env, "java/util/HashMap");
    jmethodID map_init = (*env)->GetMethodID(env, map_class, "<init>", "()V");
    jmethodID map_put = (*env)->GetMethodID(env, map_class, "put", 
                                            "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    jobject payload_map = (*env)->NewObject(env, map_class, map_init);
    jstring message_key = (*env)->NewStringUTF(env, "message");
    jstring message_value = (*env)->NewStringUTF(env, message ? message : "Unknown error");
    (*env)->CallObjectMethod(env, payload_map, map_put, message_key, message_value);
    (*env)->DeleteLocalRef(env, message_key);
    (*env)->DeleteLocalRef(env, message_value);
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in error event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            ALOGI("✅ Error event emitted: %s", message);
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    (*env)->DeleteLocalRef(env, payload_map);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

// 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - эмит события изменения AudioState
void native_player_emit_audio_state_event(const char *state) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit audioState event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: audioState event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit audioState event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit audioState event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "audioState");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём payload с state
    jclass map_class = (*env)->FindClass(env, "java/util/HashMap");
    jmethodID map_init = (*env)->GetMethodID(env, map_class, "<init>", "()V");
    jmethodID map_put = (*env)->GetMethodID(env, map_class, "put", 
                                            "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    jobject payload_map = (*env)->NewObject(env, map_class, map_init);
    jstring state_key = (*env)->NewStringUTF(env, "state");
    jstring state_value = (*env)->NewStringUTF(env, state);
    (*env)->CallObjectMethod(env, payload_map, map_put, state_key, state_value);
    (*env)->DeleteLocalRef(env, state_key);
    (*env)->DeleteLocalRef(env, state_value);
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in audioState event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            ALOGI("✅ audioState event emitted: %s", state);
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    (*env)->DeleteLocalRef(env, payload_map);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔒 Native Event Contract: paused эмитится ТОЛЬКО если был playing и воспроизведение фактически остановлено
void native_player_emit_paused_event(void) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit paused event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: paused event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit paused event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit paused event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "paused");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jobject payload_map = NULL;
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in paused event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            ALOGI("✅ Paused event emitted to Flutter");
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔒 Native Event Contract: completed эмитится вместо paused при EOF
/// 🔥 КРИТИЧЕСКИЙ FIX: NEXT VIDEO - completed эмитится РОВНО 1 РАЗ
/// Используем флаг completed_emitted для предотвращения дубликатов
// 🔥 PATCH 4: Вспомогательная функция для добавления playerToken в payload
static void add_player_token_to_payload(JNIEnv *env, jobject payload_map, int player_token) {
    if (!payload_map) {
        return; // Если payload_map NULL, ничего не делаем
    }
    
    // Получаем класс HashMap
    jclass map_class = (*env)->GetObjectClass(env, payload_map);
    if (!map_class) {
        ALOGW("⚠️ Failed to get HashMap class for adding playerToken");
        return;
    }
    
    // Получаем метод put
    jmethodID put_method = (*env)->GetMethodID(env, map_class, "put", 
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    if (!put_method) {
        ALOGW("⚠️ Failed to get put method for adding playerToken");
        (*env)->DeleteLocalRef(env, map_class);
        return;
    }
    
    // Создаём ключ "playerToken"
    jstring token_key = (*env)->NewStringUTF(env, "playerToken");
    if (!token_key) {
        ALOGW("⚠️ Failed to create playerToken key string");
        (*env)->DeleteLocalRef(env, map_class);
        return;
    }
    
    // Создаём значение (player_token как строка)
    char token_str[32];
    snprintf(token_str, sizeof(token_str), "%d", player_token);
    jstring token_value = (*env)->NewStringUTF(env, token_str);
    if (!token_value) {
        ALOGW("⚠️ Failed to create playerToken value string");
        (*env)->DeleteLocalRef(env, token_key);
        (*env)->DeleteLocalRef(env, map_class);
        return;
    }
    
    // Добавляем playerToken в payload
    (*env)->CallObjectMethod(env, payload_map, put_method, token_key, token_value);
    
    // Освобождаем локальные ссылки
    (*env)->DeleteLocalRef(env, token_key);
    (*env)->DeleteLocalRef(env, token_value);
    (*env)->DeleteLocalRef(env, map_class);
}

void native_player_emit_completed_event(void) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit completed event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: NEXT VIDEO - проверяем, что completed ещё не эмитился
    // ❌ Без этого будет: двойной next, race condition, пропуски видео
    if (g_completed_emitted) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ NEXT VIDEO: completed already emitted, ignoring duplicate");
        return;
    }
    g_completed_emitted = 1;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: completed event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        g_completed_emitted = 0; // Сбрасываем флаг если dispose заблокировал событие
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit completed event");
        pthread_mutex_unlock(&g_jni_mutex);
        g_completed_emitted = 0; // Сбрасываем флаг если callback не готов
        return;
    }
    
    // 🔥 PATCH 4: Получаем playerToken из PlayerContext
    int player_token = 0;
    if (g_player_context) {
        player_token = g_player_context->player_token;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit completed event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "playbackCompleted"); // 🔥 PATCH 4: Изменено на playbackCompleted
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // 🔥 PATCH 4: Создаём payload_map с playerToken и reason
    jclass hashmap_class = (*env)->FindClass(env, "java/util/HashMap");
    if (!hashmap_class) {
        ALOGE("❌ Failed to find HashMap class");
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jmethodID hashmap_init = (*env)->GetMethodID(env, hashmap_class, "<init>", "()V");
    jmethodID hashmap_put = (*env)->GetMethodID(env, hashmap_class, "put", 
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    if (!hashmap_init || !hashmap_put) {
        ALOGE("❌ Failed to get HashMap methods");
        (*env)->DeleteLocalRef(env, hashmap_class);
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jobject payload_map = (*env)->NewObject(env, hashmap_class, hashmap_init);
    if (!payload_map) {
        ALOGE("❌ Failed to create HashMap");
        (*env)->DeleteLocalRef(env, hashmap_class);
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Добавляем playerToken
    add_player_token_to_payload(env, payload_map, player_token);
    
    // Добавляем reason="eof"
    jstring reason_key = (*env)->NewStringUTF(env, "reason");
    jstring reason_value = (*env)->NewStringUTF(env, "eof");
    if (reason_key && reason_value) {
        (*env)->CallObjectMethod(env, payload_map, hashmap_put, reason_key, reason_value);
        (*env)->DeleteLocalRef(env, reason_key);
        (*env)->DeleteLocalRef(env, reason_value);
    }
    
    (*env)->DeleteLocalRef(env, hashmap_class);
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in playbackCompleted event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            ALOGI("✅ playbackCompleted event emitted to Flutter (playerToken=%d)", player_token);
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    (*env)->DeleteLocalRef(env, payload_map);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

// Forward declarations для функций эмита событий
void native_player_emit_play_started_event(void);
void native_player_emit_paused_event(void);
void native_player_emit_completed_event(void);
void native_player_emit_play_accepted_event(void);
void native_player_emit_decode_started_event(void);
void native_player_emit_egl_context_lost_event(void);
void native_player_emit_surface_replaced_event(void);

/// Устанавливает VideoRenderGL instance (вызывается из video_render_gl_init)
void native_player_set_renderer(VideoRenderGL *renderer) {
    g_renderer = renderer;
    ALOGI("✅ native_player_set_renderer: Renderer set to %p", (void *)renderer);
}

/// Очищает все глобальные ссылки (вызывается при завершении)
/// 🔥 КРИТИЧЕСКИЙ FIX: NEXT VIDEO - сброс флага completed_emitted
/// Вызывается при dispose для сброса флага перед открытием нового видео
void native_player_reset_completed_flag(void) {
    // g_completed_emitted объявлена как static в этом же файле, extern не нужен
    g_completed_emitted = 0;
    ALOGI("✅ NEXT VIDEO: completed_emitted flag reset");
}

void native_player_cleanup(void) {
    pthread_mutex_lock(&g_jni_mutex);
    
    // Освобождаем event callback
    if (g_event_callback && g_jvm) {
        JNIEnv *env = NULL;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) {
            (*env)->DeleteGlobalRef(env, g_event_callback);
        }
        g_event_callback = NULL;
        g_on_event_method = NULL;
    }
    
    // Освобождаем plugin instance
    if (g_plugin && g_jvm) {
        JNIEnv *env = NULL;
        if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) {
            (*env)->DeleteGlobalRef(env, g_plugin);
        }
        g_plugin = NULL;
        g_onFrameAvailable = NULL;
    }
    
    // Очищаем renderer (но НЕ освобождаем память - это делает nativeDisposePlayerContext)
    g_renderer = NULL;
    
    // 🔒 Native Event Contract: сбрасываем флаг duration
    g_duration_emitted = 0;
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    ALOGI("✅ native_player_cleanup: All global references cleared");
}

// ================= JNI_OnLoad =================
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_jvm = vm;
    ALOGI("✅ JNI_OnLoad: JavaVM stored");
    return JNI_VERSION_1_6;
}

// ================= Event Emission Functions =================

/// 🔴 ЭТАЛОН: Отправить prepared event с has_audio и duration
/// 🔒 ШАГ I: prepared эмитится строго 1 раз (проверка через ctx->prepared_emitted)
void native_player_emit_prepared_event_with_data(PlayerContext *ctx, int has_audio, int64_t duration_ms) {
    if (!ctx) {
        ALOGW("⚠️ Cannot emit prepared event - PlayerContext is NULL");
        return;
    }
    
    // 🔒 FIX Z11: prepared эмитится строго 1 раз (контракт)
    if (ctx->prepared_emitted) {
        ALOGW("⚠️ Prepared event already emitted, skipping duplicate");
        return;
    }
    
    // 🔒 FIX Z8/Z11: prepared НЕ ЖДЁТ duration (duration может быть 0, обновится позже)
    // Для video-only: duration может быть 0 до demux EOF, это нормально
    // prepared отправляется сразу после streams найдены и decoder готов
    // duration == 0 допустимо, FSM примет prepared и обновит duration позже
    if (duration_ms < 0) {
        ALOGW("⚠️ Prepared event with negative duration (%lld ms), using 0", (long long)duration_ms);
        duration_ms = 0;
    }
    
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit prepared event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: prepared event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        // 🔥 КРИТИЧЕСКИЙ FIX: Буферируем prepared событие, если callback не зарегистрирован
        // Это гарантирует, что prepared не потеряется, если оно пришло до onListen
        g_prepared_pending = 1;
        g_prepared_has_audio = has_audio;
        g_prepared_duration_ms = duration_ms;
        ALOGW("⚠️ Event callback not registered, buffering prepared event (has_audio=%d, duration=%lld ms)", 
              has_audio, (long long)duration_ms);
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit prepared event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "prepared");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // 🔒 Native Event Contract: duration эмитится строго один раз, ТОЛЬКО если > 0
    // Эмитим duration ПЕРЕД prepared, если ещё не эмитили
    if (!g_duration_emitted && duration_ms > 0) {
        native_player_emit_duration_event(duration_ms);
    }
    
    // Создаём HashMap для payload
    jclass hashMapClass = (*env)->FindClass(env, "java/util/HashMap");
    if (!hashMapClass) {
        ALOGE("❌ Failed to find HashMap class");
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jmethodID hashMapInit = (*env)->GetMethodID(env, hashMapClass, "<init>", "()V");
    jmethodID hashMapPut = (*env)->GetMethodID(env, hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    jobject payload_map = (*env)->NewObject(env, hashMapClass, hashMapInit);
    if (!payload_map) {
        ALOGE("❌ Failed to create HashMap");
        (*env)->DeleteLocalRef(env, hashMapClass);
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Добавляем hasAudio (Boolean)
    jstring hasAudioKey = (*env)->NewStringUTF(env, "hasAudio");
    jclass booleanClass = (*env)->FindClass(env, "java/lang/Boolean");
    jmethodID booleanValueOf = (*env)->GetStaticMethodID(env, booleanClass, "valueOf", "(Z)Ljava/lang/Boolean;");
    jobject hasAudioValue = (*env)->CallStaticObjectMethod(env, booleanClass, booleanValueOf, has_audio ? JNI_TRUE : JNI_FALSE);
    (*env)->CallObjectMethod(env, payload_map, hashMapPut, hasAudioKey, hasAudioValue);
    (*env)->DeleteLocalRef(env, hasAudioKey);
    (*env)->DeleteLocalRef(env, hasAudioValue);
    (*env)->DeleteLocalRef(env, booleanClass);
    
    // Добавляем duration (Long)
    jstring durationKey = (*env)->NewStringUTF(env, "duration");
    jclass longClass = (*env)->FindClass(env, "java/lang/Long");
    jmethodID longValueOf = (*env)->GetStaticMethodID(env, longClass, "valueOf", "(J)Ljava/lang/Long;");
    jobject durationValue = (*env)->CallStaticObjectMethod(env, longClass, longValueOf, (jlong)duration_ms);
    (*env)->CallObjectMethod(env, payload_map, hashMapPut, durationKey, durationValue);
    (*env)->DeleteLocalRef(env, durationKey);
    (*env)->DeleteLocalRef(env, durationValue);
    (*env)->DeleteLocalRef(env, longClass);
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in prepared event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            // 🔒 FIX Z11: Устанавливаем флаг ПЕРЕД отправкой (защита от race condition)
            ctx->prepared_emitted = 1;
            // 🔥 КРИТИЧЕСКИЙ FIX: Если prepared был в буфере, очищаем буфер
            if (g_prepared_pending) {
                g_prepared_pending = 0;
                g_prepared_has_audio = 0;
                g_prepared_duration_ms = -1;
            }
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC Watchdog - НЕ запускаем здесь
            // Watchdog должен стартовать ТОЛЬКО после play(), когда clocks начали тикать
            // Иначе для video-only файлов watchdog будет считать idle clock как stall
            
            // 🔒 FIX Z8: duration может быть 0 для video-only (обновится после demux EOF)
            ALOGI("✅ Prepared event emitted (duration=%lld ms, audio=%d, video-only=%d)", 
                  (long long)duration_ms, has_audio, !has_audio);
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, hashMapClass);
    (*env)->DeleteLocalRef(env, payload_map);
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔴 ЭТАЛОН: Отправить duration в Flutter
void native_player_emit_duration_event(int64_t duration_ms) {
    // 🔒 Native Event Contract: duration эмитится строго один раз, ТОЛЬКО если > 0
    if (duration_ms <= 0) {
        ALOGW("⚠️ Duration is invalid (%lld ms), skipping", (long long)duration_ms);
        return;
    }
    
    if (g_duration_emitted) {
        ALOGW("⚠️ Duration already emitted, skipping duplicate");
        return;
    }
    
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit duration event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: duration event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Буферизуем duration событие, если EventChannel ещё не подписан
    if (!g_event_callback || !g_on_event_method) {
        // Сохраняем duration в буфер для отправки после onListen
        g_duration_pending_ms = duration_ms;
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGW("⚠️ Event callback not registered, buffering duration event (%lld ms)", (long long)duration_ms);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit duration event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "duration");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём HashMap для payload
    jclass hashMapClass = (*env)->FindClass(env, "java/util/HashMap");
    if (!hashMapClass) {
        ALOGE("❌ Failed to find HashMap class");
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jmethodID hashMapInit = (*env)->GetMethodID(env, hashMapClass, "<init>", "()V");
    jmethodID hashMapPut = (*env)->GetMethodID(env, hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    jobject payload_map = (*env)->NewObject(env, hashMapClass, hashMapInit);
    if (!payload_map) {
        ALOGE("❌ Failed to create HashMap");
        (*env)->DeleteLocalRef(env, hashMapClass);
        (*env)->DeleteLocalRef(env, event_type);
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Добавляем value (Long)
    jstring valueKey = (*env)->NewStringUTF(env, "value");
    jclass longClass = (*env)->FindClass(env, "java/lang/Long");
    jmethodID longValueOf = (*env)->GetStaticMethodID(env, longClass, "valueOf", "(J)Ljava/lang/Long;");
    jobject valueObj = (*env)->CallStaticObjectMethod(env, longClass, longValueOf, (jlong)duration_ms);
    (*env)->CallObjectMethod(env, payload_map, hashMapPut, valueKey, valueObj);
    (*env)->DeleteLocalRef(env, valueKey);
    (*env)->DeleteLocalRef(env, valueObj);
    (*env)->DeleteLocalRef(env, longClass);
    
    // 🔒 Native Event Contract: устанавливаем флаг ПЕРЕД эмитом
    g_duration_emitted = 1;
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in duration event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
            // Откатываем флаг при ошибке
            g_duration_emitted = 0;
        } else {
            ALOGI("✅ Duration event emitted to Flutter: %lld ms", (long long)duration_ms);
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, hashMapClass);
    (*env)->DeleteLocalRef(env, payload_map);
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔥 КРИТИЧЕСКИЙ FIX: Отправить surface_ready event в Flutter
///
/// Вызывается из render loop ПОСЛЕ успешного eglMakeCurrent().
/// Это критично для TEXTURE-RACE fix - render loop должен стартовать ТОЛЬКО после eglMakeCurrent.
/// surfaceReady = EGLSurface создан и eglMakeCurrent успешно выполнен.
void native_player_emit_surface_ready_event(void) {
    // 🔥 КРИТИЧЕСКИЙ FIX: ASSERT - surfaceReady требует renderer
    extern VideoRenderGL *g_renderer;
    if (!g_renderer) {
        __android_log_assert(
            "SURFACE",
            "NativePlayer",
            "ASSERT: surfaceReady without renderer"
        );
        ALOGE("❌ ASSERT: surfaceReady without renderer");
        return;
    }
    
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit surfaceReady event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: surfaceReady event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit surfaceReady event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit surfaceReady event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "surfaceReady");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём пустой payload
    jobject payload_map = NULL;
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in surfaceReady event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            ALOGI("✅ surfaceReady event emitted (EGLSurface ready, eglMakeCurrent successful)");
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔒 FIX Z25: Отправить first_frame event в Flutter
///
/// Вызывается из render loop ПОСЛЕ eglSwapBuffers(), когда первый кадр реально отрисован.
/// Это критично для скрытия loader в UI - loader скрывается ТОЛЬКО после реального рендера первого кадра.
/// prepared ≠ first frame - prepared означает metadata OK, first_frame означает кадр на экране.
void native_player_emit_first_frame_event(void) {
    // 🔥 КРИТИЧЕСКИЙ FIX: ASSERT - firstFrame требует renderer и avsync_gate_open
    extern VideoRenderGL *g_renderer;
    if (!g_renderer) {
        __android_log_assert(
            "FIRSTFRAME",
            "NativePlayer",
            "ASSERT: firstFrame without renderer"
        );
        ALOGE("❌ ASSERT: firstFrame without renderer");
        return;
    }
    
    if (g_player_context && !g_player_context->avsync_gate_open) {
        __android_log_assert(
            "FIRSTFRAME",
            "NativePlayer",
            "ASSERT: firstFrame without avsync_gate_open"
        );
        ALOGE("❌ ASSERT: firstFrame without avsync_gate_open");
        return;
    }
    
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit first_frame event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - не эмитим события если dispose в процессе или завершён
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: firstFrame event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    // 🔥 FIX: firstFrame эмитится строго один раз
    if (g_first_frame_emitted) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ Duplicate 'firstFrame' event ignored (already emitted)");
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        ALOGW("⚠️ Event callback not registered, cannot emit first_frame event");
        pthread_mutex_unlock(&g_jni_mutex);
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit first_frame event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "firstFrame");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём пустой payload (или NULL)
    jobject payload_map = NULL;
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in first_frame event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            // 🔥 FIX: Устанавливаем флаг ПОСЛЕ успешной отправки
            g_first_frame_emitted = 1;
            ALOGI("✅ First frame event emitted to Flutter");
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔥 КРИТИЧЕСКИЙ FIX: Отправить firstFrameAfterSeek event в Flutter
///
/// Вызывается из render loop ПОСЛЕ eglSwapBuffers(), когда первый кадр после seek реально отрисован.
/// Это критично для AVI/FLV - seek должен ждать реального кадра >= target перед переходом в ready/playing.
void native_player_emit_first_frame_after_seek_event(void) {
    if (!g_jvm) {
        ALOGW("⚠️ Cannot emit firstFrameAfterSeek event - JVM not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - блокируем эмиссию событий во время dispose
    if (g_dispose_in_progress || g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGD("⚠️ DISPOSE-GATE: firstFrameAfterSeek event blocked (dispose in progress=%d, disposed=%d)", 
              g_dispose_in_progress, g_disposed);
        return;
    }
    
    if (!g_event_callback || !g_on_event_method) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGW("⚠️ Event callback not registered, cannot emit firstFrameAfterSeek event");
        return;
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit firstFrameAfterSeek event");
            return;
        }
        need_detach = 1;
    }
    
    jstring event_type = (*env)->NewStringUTF(env, "firstFrameAfterSeek");
    if (!event_type) {
        ALOGE("❌ Failed to create event type string");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Создаём пустой payload (или NULL)
    jobject payload_map = NULL;
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in firstFrameAfterSeek event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        } else {
            ALOGI("✅ First frame after seek event emitted to Flutter");
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

// ================= JNI Methods =================

JNIEXPORT jlong JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeCreatePlayerContext(
    JNIEnv *env, jobject thiz, jstring path, jint playerToken) {
    if (!path) {
        ALOGE("❌ nativeCreatePlayerContext: path is NULL");
        return 0L;
    }
    
    const char *path_str = (*env)->GetStringUTFChars(env, path, NULL);
    if (!path_str) {
        ALOGE("❌ nativeCreatePlayerContext: Failed to get path string");
        return 0L;
    }
    
    ALOGI("🔄 nativeCreatePlayerContext: path=%s, playerToken=%d", path_str, playerToken);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: NEXT VIDEO - сбрасываем флаг completed_emitted при создании нового контекста
    // Это гарантирует, что completed может быть эмитирован для нового видео
    g_completed_emitted = 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - проверяем, что dispose завершён
    // prepare() запрещён, пока dispose не завершён на 100% (после join всех threads)
    pthread_mutex_lock(&g_jni_mutex);
    if (g_dispose_in_progress || !g_disposed) {
        pthread_mutex_unlock(&g_jni_mutex);
        ALOGE("❌ DISPOSE-GATE: prepare blocked - dispose in progress=%d, disposed=%d", 
              g_dispose_in_progress, g_disposed);
        __android_log_assert(
            "LIFECYCLE",
            "NativePlayer",
            "ASSERT: prepare() called during dispose or before dispose complete"
        );
        (*env)->ReleaseStringUTFChars(env, path, path_str);
        return 0L;
    }
    // Помечаем как active (не disposed)
    g_disposed = 0;
    pthread_mutex_unlock(&g_jni_mutex);
    
    ALOGI("✅ DISPOSE-GATE: prepare allowed (dispose complete, creating new PlayerContext)");
    
    // 🔒 Native Event Contract: сбрасываем флаги событий при создании нового контекста
    g_duration_emitted = 0;
    g_first_frame_emitted = 0;  // 🔥 FIX: Сбрасываем флаг firstFrame для нового PlayerContext
    g_play_started_emitted = 0;  // 🔥 FIX: Сбрасываем флаг playStarted для нового PlayerContext
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Сбрасываем буфер событий при создании нового контекста
    g_prepared_pending = 0;
    g_prepared_has_audio = 0;
    g_prepared_duration_ms = -1;
    g_duration_pending_ms = -1;
    
    // Создаём PlayerContext
    PlayerContext *ctx = (PlayerContext *)calloc(1, sizeof(PlayerContext));
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 10.1: Инициализация seek_serial
    // Seek serial = 0 означает "начальная эпоха" (до первого seek)
    atomic_init(&ctx->seek_serial, 0);
    if (!ctx) {
        ALOGE("❌ nativeCreatePlayerContext: Failed to allocate PlayerContext");
        (*env)->ReleaseStringUTFChars(env, path, path_str);
        return 0L;
    }
    
    // Инициализируем PlayerState
    player_state_init(&ctx->state);
    
    // Сохраняем JVM для callbacks
    ctx->jvm = g_jvm;
    
    // 🔒 DIFF 2: Инициализируем флаг play_requested
    ctx->play_requested = 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.1: Инициализация playback_mode
    // По умолчанию MODE_AV (обычный режим)
    ctx->playback_mode = MODE_AV;
    
    // 🔥 PATCH 4: Устанавливаем playerToken в PlayerContext
    ctx->player_token = playerToken;
    ALOGI("🎬 nativeCreatePlayerContext: playerToken=%d установлен в PlayerContext", playerToken);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Инициализируем AVSYNC-GATE (закрыт до surfaceReady)
    ctx->avsync_gate_open = 0;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 15.7: Инициализация pending seek
    ctx->has_pending_seek = false;
    ctx->pending_seek_seconds = 0.0;
    ctx->pending_seek_exact = false;
    
    // Открываем медиафайл
    int ret = open_media(ctx, path_str);
    if (ret < 0) {
        ALOGE("❌ nativeCreatePlayerContext: Failed to open media: %d", ret);
        free(ctx);
        (*env)->ReleaseStringUTFChars(env, path, path_str);
        return 0L;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: ASSERT - prepared ≠ playable
    // prepared == streams opened, но это не означает, что видео можно воспроизвести
    if (ctx->videoStream < 0) {
        __android_log_assert(
            "PREPARED",
            "NativePlayer",
            "ASSERT: prepared without video stream (videoStream=%d)",
            ctx->videoStream
        );
        ALOGE("❌ ASSERT: prepared without video stream");
        free(ctx);
        (*env)->ReleaseStringUTFChars(env, path, path_str);
        return 0L;
    }
    
    if (!ctx->video || !ctx->video->codecCtx) {
        __android_log_assert(
            "PREPARED",
            "NativePlayer",
            "ASSERT: prepared without video codec context"
        );
        ALOGE("❌ ASSERT: prepared without video codec context");
        free(ctx);
        (*env)->ReleaseStringUTFChars(env, path, path_str);
        return 0L;
    }
    
    // Инициализируем VideoRenderGL (если есть видео)
    if (ctx->video && ctx->video->codecCtx) {
        extern VideoRenderGL *g_renderer;
        if (!g_renderer) {
            AVRational time_base = ctx->fmt->streams[ctx->videoStream]->time_base;
            int width = ctx->video->codecCtx->width;
            int height = ctx->video->codecCtx->height;
            
            g_renderer = (VideoRenderGL *)calloc(1, sizeof(VideoRenderGL));
            if (g_renderer) {
                ret = video_render_gl_init(g_renderer, g_jvm, width, height, time_base);
                if (ret < 0) {
                    ALOGE("❌ nativeCreatePlayerContext: Failed to init VideoRenderGL");
                    free(g_renderer);
                    g_renderer = NULL;
                } else {
                    native_player_set_renderer(g_renderer);
                    ALOGI("✅ nativeCreatePlayerContext: VideoRenderGL initialized");
                }
            }
        }
    }
    
    (*env)->ReleaseStringUTFChars(env, path, path_str);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Сохраняем глобальную ссылку для ASSERT-ов
    g_player_context = ctx;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Отправляем буферизованное prepared событие, если оно было и callback зарегистрирован
    // Это гарантирует, что prepared не потеряется, если оно пришло до регистрации callback
    pthread_mutex_lock(&g_jni_mutex);
    int prepared_pending = g_prepared_pending;
    int prepared_has_audio = g_prepared_has_audio;
    int64_t prepared_duration_ms = g_prepared_duration_ms;
    int callback_ready = (g_event_callback != NULL && g_on_event_method != NULL);
    pthread_mutex_unlock(&g_jni_mutex);
    
    if (prepared_pending && prepared_duration_ms >= 0 && callback_ready) {
        ALOGI("🔄 nativeCreatePlayerContext: Sending buffered prepared event after context creation (has_audio=%d, duration=%lld ms)", 
              prepared_has_audio, (long long)prepared_duration_ms);
        native_player_emit_prepared_event_with_data(ctx, prepared_has_audio, prepared_duration_ms);
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC Watchdog - НЕ запускаем здесь
        // Watchdog должен стартовать ТОЛЬКО после play(), когда clocks начали тикать
        // Иначе для video-only файлов watchdog будет считать idle clock как stall
        
        // Очищаем буфер после отправки
        pthread_mutex_lock(&g_jni_mutex);
        g_prepared_pending = 0;
        g_prepared_has_audio = 0;
        g_prepared_duration_ms = -1;
        pthread_mutex_unlock(&g_jni_mutex);
    }
    
    ALOGI("✅ nativeCreatePlayerContext: PlayerContext created: %p", (void *)ctx);
    
    return (jlong)ctx;
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativePlay(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativePlay: PlayerContext is NULL");
        return;
    }
    
    ALOGI("▶️▶️▶️ nativePlay CALLED: PlayerContext=%p", (void *)ctx);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: ASSERT - play() не должен вызываться после dispose
    if (g_dispose_in_progress || g_disposed) {
        __android_log_assert(
            "AVSYNC",
            "NativePlayer",
            "ASSERT: play() called during dispose or after disposed"
        );
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: PLAY-GATE - play() ДОЛЖЕН вызываться ТОЛЬКО ПОСЛЕ surfaceReady
    // prepare ≠ play - prepare открывает файл, play запускает decode threads
    // Без play() decode никогда не стартует → видео не появится
    // Проверяем AVSYNC-GATE перед запуском decode
    if (!ctx->avsync_gate_open) {
        // 🔥 КРИТИЧЕСКИЙ FIX: ASSERT - play() без surfaceReady не должен стартовать decode
        if (ctx->decode_started) {
            __android_log_assert(
                "PLAY",
                "NativePlayer",
                "ASSERT: decode_started without avsync_gate_open"
            );
            ALOGE("❌ ASSERT: decode_started without avsync_gate_open");
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-GATE закрыт - устанавливаем pending_play
        // decode запустится автоматически после surfaceReady (в video_render_gl.c)
        ctx->pending_play = 1;
        ctx->play_requested = 1;
        ALOGI("⏳ PLAY-GATE: play() called before surfaceReady, pending play (decode will start after AVSYNC-GATE open)");
        return;
    }
    
    // 🔴 КРИТИЧНО: Проверяем, запущен ли decode thread
    // Если decode thread не запущен, нужно запустить его, даже если is_playing = true
    int decode_thread_running = 0;
    if (ctx->video && ctx->video->decodeThread) {
        decode_thread_running = 1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: play() idempotent - если уже playing, подтверждаем через playAccepted
    // Это важно для PLAY_ASSERT - даже повторный play() должен подтвердиться
    if (is_playing(ctx) && decode_thread_running) {
        // 🔥 КРИТИЧЕСКИЙ FIX: play() idempotent - если уже playing, эмитим playAccepted для ASSERT
        // НЕ запускаем decode повторно, НО подтверждаем что play() принят
        ALOGI("🔄 nativePlay: Already playing → emit playAccepted (idempotent, for ASSERT)");
        native_player_emit_play_accepted_event();
        if (ctx->prepared_emitted) {
            native_player_emit_play_started_event(); // Diagnostic only
        } else {
            ALOGW("⚠️ nativePlay: Already playing but prepared not emitted yet");
        }
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: play() теперь управляет ТОЛЬКО clock/pause, а не запуском decode
    // Decode стартует автоматически после surfaceReady (в video_render_gl.c)
    // Это решает FIRST-FRAME-DEADLOCK: decode не ждёт play(), play() не ждёт firstFrame
    ctx->play_requested = 1;
    ctx->pending_play = 0; // Сбрасываем pending, так как play() вызван после surfaceReady
    ALOGI("✅ PLAY-GATE: play() accepted (AVSYNC-GATE open, decode should already be started)");
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Эмитим playAccepted ПОСЛЕ проверки AVSYNC-GATE
    // Это гарантирует, что play() был вызван и прошёл AVSYNC-GATE
    native_player_emit_play_accepted_event();
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Проверяем, что decode уже стартовал (должен был стартовать после surfaceReady)
    // Если decode не стартовал - это ошибка архитектуры, но не блокируем play()
    // play() теперь только управляет clock, decode работает независимо
    if (!ctx->decode_started) {
        ALOGW("⚠️ nativePlay: Decode not started yet (should have started after surfaceReady)");
        // 🔥 КРИТИЧЕСКИЙ FIX: Fallback - запускаем decode если он не стартовал
        // Это защита от race condition, но в нормальном flow decode должен стартовать автоматически
        ctx->decode_started = 1;
        ctx->state.abort_request = 0;
        
        int ret_demux = pthread_create(&ctx->demuxThread, NULL, demux_thread, ctx);
        if (ret_demux != 0) {
            ALOGE("❌ nativePlay: Failed to create demux thread (fallback): %d", ret_demux);
            ctx->decode_started = 0;
        } else {
            ALOGI("✅ nativePlay: Demux thread started (fallback)");
            
            if (ctx->video) {
                int ret_decode = video_decode_thread_start(ctx->video, ctx->audio);
                if (ret_decode < 0) {
                    ALOGE("❌ nativePlay: Failed to start video decode thread (fallback): %d", ret_decode);
                } else {
                    ALOGI("✅ nativePlay: Decode thread started (fallback)");
                    native_player_emit_decode_started_event();
                }
            }
        }
    } else {
        ALOGI("✅ nativePlay: Decode already started (auto-started after surfaceReady)");
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Если surface уже прикреплён, можно сразу использовать renderer
    // Но это не блокирует decode - decode уже работает
    int renderer_attached = 0;
    if (g_renderer) {
        extern int video_render_gl_has_window(VideoRenderGL *renderer);
        renderer_attached = video_render_gl_has_window(g_renderer);
    }
    
    if (renderer_attached && ctx->renderer_ready) {
        ALOGI("✅ nativePlay: Surface already attached, renderer will use it for rendering");
    } else {
        ALOGI("⏳ nativePlay: Surface not attached yet, decode will buffer frames until surface ready");
    }
    
    // Запускаем воспроизведение
    int ret = play(ctx);
    if (ret < 0) {
        ALOGE("❌ nativePlay: play() failed: %d", ret);
        return;
    }
    
    // Снимаем паузу с VideoRenderGL
    if (g_renderer) {
        video_render_gl_set_paused(g_renderer, false);
    }
    
    // 🔒 FIX R: playStarted всегда после успешного play()
    // Состояние playing определяется через ctx->paused = 0 (уже установлен в play())
    // Не нужно устанавливать ctx->is_playing, т.к. это поле не существует в PlayerContext
    
    // 🔒 FSM contract: playStarted только после prepared
    if (!ctx->prepared_emitted) {
        ALOGW("⚠️ nativePlay: Cannot emit playStarted - prepared not emitted yet");
        // НЕ возвращаемся - play() уже выполнен, просто не отправляем событие
        // playStarted будет отправлен позже, когда prepared придет (но FSM игнорирует его)
    } else {
        // 🔒 FIX Z7: playStarted эмитится для диагностики (не участвует в контракте)
        // FSM игнорирует playStarted, playback started определяется через position > 0
        native_player_emit_play_started_event();
        ALOGI("✅ nativePlay: Playback started, playStarted emitted (diagnostic only)");
    }
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativePause(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativePause: PlayerContext is NULL");
        return;
    }
    
    ALOGI("⏸️ nativePause: PlayerContext=%p", (void *)ctx);
    
    // 🔒 Native Event Contract: эмитим paused ТОЛЬКО если реально playing
    if (!is_playing(ctx)) {
        ALOGD("nativePause: Not playing, skipping");
        return;
    }
    
    // Ставим на паузу
    player_pause(ctx);
    
    // Ставим паузу в VideoRenderGL
    if (g_renderer) {
        video_render_gl_set_paused(g_renderer, true);
    }
    
    // 🔒 Native Event Contract: эмитим paused ТОЛЬКО после успешной паузы
    native_player_emit_paused_event();
    
    ALOGI("✅ nativePause: Playback paused");
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSeek(
    JNIEnv *env, jobject thiz, jlong playerContext, jdouble seconds) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeSeek: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔍 nativeSeek: PlayerContext=%p, seconds=%.3f", (void *)ctx, seconds);
    
    // Выполняем точный seek
    int ret = player_seek(ctx, seconds, true);
    if (ret < 0) {
        ALOGE("❌ nativeSeek: player_seek() failed: %d", ret);
        return;
    }
    
    ALOGI("✅ nativeSeek: Seek completed");
}

/// 🔥 PATCH 11: Установка скорости воспроизведения
JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSetPlaybackSpeed(
    JNIEnv *env, jobject thiz, jlong playerContext, jfloat speed) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeSetPlaybackSpeed: PlayerContext is NULL");
        return;
    }
    
    // 🔥 PATCH 11: Валидация скорости (0.25 - 3.0)
    if (speed < 0.25f || speed > 3.0f) {
        ALOGW("⚠️ nativeSetPlaybackSpeed: Speed %.2f out of range [0.25, 3.0], clamping", speed);
        if (speed < 0.25f) {
            speed = 0.25f;
        } else if (speed > 3.0f) {
            speed = 3.0f;
        }
    }
    
    ALOGI("🔄 nativeSetPlaybackSpeed: PlayerContext=%p, speed=%.2fx", (void *)ctx, speed);
    
    // Вызываем player_set_speed для применения скорости
    int ret = player_set_speed(ctx, (double)speed);
    if (ret < 0) {
        ALOGE("❌ nativeSetPlaybackSpeed: player_set_speed() failed: %d", ret);
        return;
    }
    
    ALOGI("✅ nativeSetPlaybackSpeed: Speed set to %.2fx", speed);
}

JNIEXPORT jlong JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetDuration(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetDuration: PlayerContext is NULL");
        return 0L;
    }
    
    int64_t duration_ms = get_duration(ctx);
    return (jlong)duration_ms;
}

JNIEXPORT jdouble JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetPosition(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetPosition: PlayerContext is NULL");
        return 0.0;
    }
    
    // 🔒 Native Event Contract: position всегда >= 0
    int64_t position_ms = get_position(ctx);
    if (position_ms < 0) {
        position_ms = 0;
    }
    
    return (jdouble)(position_ms / 1000.0); // Возвращаем в секундах
}

/// 🔥 КРИТИЧЕСКИЙ FIX: RENDER_STALL_ASSERT - получение timestamp последнего успешного eglSwapBuffers
/// Используется для проверки, что кадры реально обновляются во время playing
JNIEXPORT jlong JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetLastRenderTs(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetLastRenderTs: PlayerContext is NULL");
        return 0;
    }
    
    return ctx->last_render_ts_ms;
}

// 🔥 КРИТИЧЕСКИЙ FIX: AUDIO_DRIFT_ASSERT - получение video и audio clock
JNIEXPORT jdouble JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetVideoClock(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        return 0.0;
    }
    // video_clock = master_clock_ms / 1000.0 (в секундах)
    return ctx->master_clock_ms / 1000.0;
}

JNIEXPORT jdouble JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetAudioClock(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        return 0.0;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: AUDIO CLOCK SOURCE FIX - ШАГ 16: Используем audio_get_clock() (канонический)
    extern double audio_get_clock(AudioState *as);
    if (ctx->audio) {
        double audio_clock_sec = audio_get_clock(ctx->audio);
        return isnan(audio_clock_sec) ? 0.0 : audio_clock_sec;
    }
    
    return 0.0;
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeDisposePlayerContext(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeDisposePlayerContext: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🛑 nativeDisposePlayerContext: PlayerContext=%p", (void *)ctx);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - атомарный dispose с join всех потоков
    // prepare() запрещён, пока dispose не завершён на 100% (после join всех threads)
    pthread_mutex_lock(&g_jni_mutex);
    g_dispose_in_progress = 1;
    g_disposed = 0;  // Помечаем как не disposed (dispose в процессе)
    pthread_mutex_unlock(&g_jni_mutex);
    
    ALOGI("🛑 DISPOSE-GATE: dispose begin (prepare blocked until complete)");
    
    // 🔒 Native Event Contract: сбрасываем флаг duration при dispose
    g_duration_emitted = 0;
    
    // Останавливаем все потоки и освобождаем ресурсы
    // player_shutdown() делает join всех потоков (decode, render, demux)
    ALOGI("🛑 DISPOSE-GATE: Stopping all threads...");
    player_shutdown(ctx);
    ALOGI("✅ DISPOSE-GATE: All threads stopped (join complete)");
    
    // Освобождаем VideoRenderGL
    if (g_renderer) {
        video_render_gl_release(g_renderer);
        free(g_renderer);
        g_renderer = NULL;
        native_player_set_renderer(NULL);
    }
    
    // Освобождаем PlayerContext
    free(ctx);
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Сбрасываем глобальную ссылку для ASSERT-ов
    g_player_context = NULL;
    
    // 🔥 КРИТИЧЕСКИЙ FIX: DISPOSE-GATE - помечаем dispose завершённым
    // Теперь prepare() разрешён
    pthread_mutex_lock(&g_jni_mutex);
    g_disposed = 1;
    g_dispose_in_progress = 0;
    pthread_mutex_unlock(&g_jni_mutex);
    
    ALOGI("✅ DISPOSE-GATE: dispose complete (prepare allowed now)");
    ALOGI("✅ nativeDisposePlayerContext: PlayerContext disposed");
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSetEventCallback(
    JNIEnv *env, jobject thiz, jobject callback) {
    pthread_mutex_lock(&g_jni_mutex);
    
    // Освобождаем старый callback, если есть
    if (g_event_callback) {
        (*env)->DeleteGlobalRef(env, g_event_callback);
        g_event_callback = NULL;
        g_on_event_method = NULL;
    }
    
    // Сохраняем новый callback
    if (callback) {
        // Получаем класс callback
        jclass callbackClass = (*env)->GetObjectClass(env, callback);
        if (!callbackClass) {
            ALOGE("❌ nativeSetEventCallback: Failed to get callback class");
            pthread_mutex_unlock(&g_jni_mutex);
            return;
        }
        
        // Получаем methodID для onEvent
        g_on_event_method = (*env)->GetMethodID(env, callbackClass, "onEvent", "(Ljava/lang/String;Ljava/util/Map;)V");
        if (!g_on_event_method) {
            ALOGE("❌ nativeSetEventCallback: Failed to get onEvent method");
            (*env)->DeleteLocalRef(env, callbackClass);
            pthread_mutex_unlock(&g_jni_mutex);
            return;
        }
        
        // Создаём GlobalRef
        g_event_callback = (*env)->NewGlobalRef(env, callback);
        if (!g_event_callback) {
            ALOGE("❌ nativeSetEventCallback: Failed to create GlobalRef");
            g_on_event_method = NULL;
            (*env)->DeleteLocalRef(env, callbackClass);
            pthread_mutex_unlock(&g_jni_mutex);
            return;
        }
        
        (*env)->DeleteLocalRef(env, callbackClass);
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Помечаем EventChannel как готовый
        g_event_listener_ready = 1;
        ALOGI("✅ nativeSetEventCallback: Event callback registered (EventChannel ready)");
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Отправляем накопленное duration событие, если есть
        int64_t duration_pending = g_duration_pending_ms;
        if (duration_pending > 0) {
            pthread_mutex_unlock(&g_jni_mutex);
            ALOGI("🔄 nativeSetEventCallback: Sending buffered duration event (%lld ms)", (long long)duration_pending);
            native_player_emit_duration_event(duration_pending);
            pthread_mutex_lock(&g_jni_mutex);
            g_duration_pending_ms = -1;  // Очищаем буфер
        }
        
        // 🔥 КРИТИЧЕСКИЙ FIX: Отправляем буферизованное prepared событие, если есть
        int prepared_pending = g_prepared_pending;
        int prepared_has_audio = g_prepared_has_audio;
        int64_t prepared_duration_ms = g_prepared_duration_ms;
        PlayerContext *ctx_for_prepared = g_player_context;  // Используем глобальный контекст
        if (prepared_pending && prepared_duration_ms >= 0 && ctx_for_prepared) {
            pthread_mutex_unlock(&g_jni_mutex);
            ALOGI("🔄 nativeSetEventCallback: Sending buffered prepared event (has_audio=%d, duration=%lld ms)", 
                  prepared_has_audio, (long long)prepared_duration_ms);
            native_player_emit_prepared_event_with_data(ctx_for_prepared, prepared_has_audio, prepared_duration_ms);
            
            // 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC Watchdog - НЕ запускаем здесь
            // Watchdog должен стартовать ТОЛЬКО после play(), когда clocks начали тикать
            // Иначе для video-only файлов watchdog будет считать idle clock как stall
            
            pthread_mutex_lock(&g_jni_mutex);
            g_prepared_pending = 0;  // Очищаем буфер
            g_prepared_has_audio = 0;
            g_prepared_duration_ms = -1;
        } else if (prepared_pending) {
            // Если prepared был в буфере, но контекст ещё не создан, просто очищаем буфер
            // prepared будет отправлен позже, когда контекст будет готов
            ALOGI("🔄 nativeSetEventCallback: Prepared event was buffered but context not ready yet (will be sent later)");
            g_prepared_pending = 0;  // Очищаем буфер, чтобы не отправлять дважды
            g_prepared_has_audio = 0;
            g_prepared_duration_ms = -1;
        }
    } else {
        g_event_listener_ready = 0;  // 🔥 FIX: Сбрасываем флаг готовности при очистке callback
        ALOGI("✅ nativeSetEventCallback: Event callback cleared");
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
}

// ================= Additional JNI Methods =================

JNIEXPORT jint JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetVideoWidth(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetVideoWidth: PlayerContext is NULL");
        return 0;
    }
    
    // Получаем ширину из codecCtx или codecpar
    if (ctx->video && ctx->video->codecCtx) {
        return ctx->video->codecCtx->width;
    } else if (ctx->videoStream >= 0 && ctx->fmt && ctx->videoStream < ctx->fmt->nb_streams) {
        AVStream *video_stream = ctx->fmt->streams[ctx->videoStream];
        if (video_stream && video_stream->codecpar) {
            return video_stream->codecpar->width;
        }
    }
    
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetVideoHeight(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetVideoHeight: PlayerContext is NULL");
        return 0;
    }
    
    // Получаем высоту из codecCtx или codecpar
    if (ctx->video && ctx->video->codecCtx) {
        return ctx->video->codecCtx->height;
    } else if (ctx->videoStream >= 0 && ctx->fmt && ctx->videoStream < ctx->fmt->nb_streams) {
        AVStream *video_stream = ctx->fmt->streams[ctx->videoStream];
        if (video_stream && video_stream->codecpar) {
            return video_stream->codecpar->height;
        }
    }
    
    return 0;
}

JNIEXPORT jboolean JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetHasAudio(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetHasAudio: PlayerContext is NULL");
        return JNI_FALSE;
    }
    
    return ctx->has_audio ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetError(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeGetError: PlayerContext is NULL");
        return 0;
    }
    
    pthread_mutex_lock(&ctx->error_mutex);
    int error = ctx->error;
    pthread_mutex_unlock(&ctx->error_mutex);
    
    return error;
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSetInterpolation(
    JNIEnv *env, jobject thiz, jlong playerContext, jint mode) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeSetInterpolation: PlayerContext is NULL");
        return;
    }
    
    if (g_renderer) {
        video_render_gl_set_interp_mode(g_renderer, mode);
        ALOGI("✅ nativeSetInterpolation: Mode set to %d", mode);
    }
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSetViewport(
    JNIEnv *env, jobject thiz, jlong playerContext, jint width, jint height) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeSetViewport: PlayerContext is NULL");
        return;
    }
    
    if (g_renderer) {
        // video_render_gl_set_viewport принимает float, float, int (rotation), int (scale_mode)
        video_render_gl_set_viewport(g_renderer, (float)width, (float)height, 0, 0);
        ALOGI("✅ nativeSetViewport: Viewport set to %dx%d", width, height);
    }
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSetFitMode(
    JNIEnv *env, jobject thiz, jlong playerContext, jint fitMode) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeSetFitMode: PlayerContext is NULL");
        return;
    }
    
    if (g_renderer) {
        video_render_gl_set_fit_mode(g_renderer, fitMode);
        ALOGI("✅ nativeSetFitMode: Fit mode set to %d", fitMode);
    }
}

JNIEXPORT jint JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeStartRenderLoop(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeStartRenderLoop: PlayerContext is NULL");
        return -1;
    }
    
    // 🔒 FIX Z34: Проверяем, что surface прикреплён перед стартом render loop
    // Render loop НЕ должен стартовать до attach surface
    if (g_renderer) {
        extern int video_render_gl_has_window(VideoRenderGL *renderer);
        if (!video_render_gl_has_window(g_renderer)) {
            ALOGW("⚠️ nativeStartRenderLoop: Surface not attached yet, render loop will start after attach");
            // НЕ возвращаем ошибку - render loop запустится автоматически в nativeAttachSurfaceTexture
            return 0;
        }
    }
    
    // Используем функцию из ffmpeg_player_lifecycle.c
    int ret = render_loop_start(ctx);
    if (ret < 0) {
        ALOGE("❌ nativeStartRenderLoop: render_loop_start failed");
        return -1;
    }
    ALOGI("✅ nativeStartRenderLoop: Render loop started");
    return 0;
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeStopRenderLoop(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeStopRenderLoop: PlayerContext is NULL");
        return;
    }
    
    // Используем функцию из ffmpeg_player_lifecycle.c
    render_loop_stop(ctx);
    ALOGI("✅ nativeStopRenderLoop: Render loop stopped");
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativePlayerPlugin_nativeAttachSurfaceTexture(
    JNIEnv *env, jobject thiz, jlong playerContext, jlong textureId, jobject surface) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeAttachSurfaceTexture: PlayerContext is NULL");
        return;
    }
    
    if (!surface) {
        ALOGE("❌ nativeAttachSurfaceTexture: Surface is NULL");
        return;
    }
    
    // Получаем ANativeWindow из Surface
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        ALOGE("❌ nativeAttachSurfaceTexture: Failed to get ANativeWindow from Surface");
        return;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SURFACE_REPLACED ASSERT - детектируем замену Surface
    if (g_current_window && g_current_window != window) {
        ALOGE("❌ SURFACE_REPLACED detected");
        native_player_emit_surface_replaced_event();
    }
    g_current_window = window;
    
    if (g_renderer) {
        int ret = video_render_gl_attach_window(g_renderer, window);
        if (ret < 0) {
            ALOGE("❌ nativeAttachSurfaceTexture: Failed to attach window");
            ANativeWindow_release(window);
            return;
        }
        ALOGI("✅ nativeAttachSurfaceTexture: Surface attached, textureId=%lld", (long long)textureId);
        
        // 🔒 FIX Z34: Запускаем render loop ПОСЛЕ attach surface (критично для первого кадра)
        // Render loop НЕ должен стартовать до того, как EGLSurface создан
        extern int render_loop_start(PlayerContext *ctx);
        if (!ctx->rendering) {
            ret = render_loop_start(ctx);
            if (ret < 0) {
                ALOGE("❌ nativeAttachSurfaceTexture: Failed to start render loop");
                // Откатываем attach
                video_render_gl_detach_window(g_renderer);
                ANativeWindow_release(window);
                return;
            }
            ALOGI("✅ nativeAttachSurfaceTexture: Render loop started after surface attach");
        } else {
            ALOGD("nativeAttachSurfaceTexture: Render loop already running");
        }
        
        // 🔒 FIX Z35: Помечаем renderer готовым ТОЛЬКО после EGLSurface + render loop
        //
        // АРХИТЕКТУРНОЕ ОБОСНОВАНИЕ:
        // ExoPlayer: Surface создаётся → MediaCodec.configure(surface) → готов к decode
        // FFmpeg: EGLSurface создаётся → render loop запущен → готов к decode
        //
        // Это эквивалент ExoPlayer's "Surface ready" состояния.
        // Decoder НЕ ИМЕЕТ ПРАВА стартовать до этого момента
        ctx->renderer_ready = 1;
        ctx->surface_attached = 1;  // 🔒 FIX: Помечаем surface как прикреплённый
        ALOGI("✅ nativeAttachSurfaceTexture: Renderer ready (EGLSurface + render loop)");
        ALOGI("   (ExoPlayer equivalent: MediaCodec.configure(surface) completed)");
        
        // 🔒 DIFF 2: Decode/demux стартует ТОЛЬКО после play() (не в attach surface)
        // Это гарантирует, что первый кадр будет декодирован когда render готов
        // и не будет потерян до готовности render loop
        // Эквивалент ExoPlayer: MediaCodec.start() после play(), не после configure(surface)
        // НЕ запускаем decode здесь - он запустится в nativePlay() после play_requested
        ALOGI("✅ nativeAttachSurfaceTexture: Surface attached, decode will start after play()");
        
        // 🔒 DIFF 2: Вызываем отложенный play, если он был запрошен
        // Теперь decode стартует ТОЛЬКО после play(), а не в attach surface
        if (ctx->pending_play) {
            ctx->pending_play = 0;
            ctx->play_requested = 1;  // 🔒 DIFF 2: Устанавливаем флаг play_requested
            ALOGI("🔄 nativeAttachSurfaceTexture: Calling pending play() after surface attach");
            
            // Вызываем nativePlay напрямую (мы уже в JNI)
            // nativePlay запустит decode/demux, так как play_requested = 1
            if (is_playing(ctx)) {
                ALOGD("nativeAttachSurfaceTexture: Already playing, skipping pending play");
            } else {
                // 🔒 DIFF 2: Запускаем decode/demux (play_requested уже установлен)
                if (!ctx->decode_started) {
                    ctx->decode_started = 1;
                    ctx->state.abort_request = 0;
                    
                    int ret_demux = pthread_create(&ctx->demuxThread, NULL, demux_thread, ctx);
                    if (ret_demux != 0) {
                        ALOGE("❌ nativeAttachSurfaceTexture: Failed to create demux thread: %d", ret_demux);
                        ctx->decode_started = 0;
                    } else {
                        ALOGI("✅ nativeAttachSurfaceTexture: Decode started for pending play");
                    }
                }
                
                // Запускаем decode thread, если не запущен
                int decode_thread_running = 0;
                if (ctx->video && ctx->video->decodeThread) {
                    decode_thread_running = 1;
                }
                
                if (!decode_thread_running && ctx->video) {
                    ALOGI("🔄 nativeAttachSurfaceTexture: Starting decode thread for pending play (renderer ready)");
                    int ret_decode = video_decode_thread_start(ctx->video, ctx->audio);
                    if (ret_decode < 0) {
                        ALOGE("❌ nativeAttachSurfaceTexture: Failed to start decode thread: %d", ret_decode);
                    } else {
                        ALOGI("✅ nativeAttachSurfaceTexture: Decode thread started");
                    }
                }
                
                // Запускаем воспроизведение
                int play_ret = play(ctx);
                if (play_ret < 0) {
                    ALOGE("❌ nativeAttachSurfaceTexture: play() failed: %d", play_ret);
                } else {
                    // Снимаем паузу с VideoRenderGL
                    video_render_gl_set_paused(g_renderer, false);
                    // 🔒 FIX Z7: playStarted эмитится для диагностики (не участвует в контракте)
                    if (ctx->prepared_emitted) {
                        native_player_emit_play_started_event();
                    }
                    ALOGI("✅ nativeAttachSurfaceTexture: Pending play() executed successfully");
                }
            }
        }
    } else {
        ALOGE("❌ nativeAttachSurfaceTexture: g_renderer is NULL");
        ANativeWindow_release(window);
    }
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativePlayerPlugin_nativeDetachSurfaceTexture(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeDetachSurfaceTexture: PlayerContext is NULL");
        return;
    }
    
    if (g_renderer) {
        video_render_gl_detach_window(g_renderer);
        ALOGI("✅ nativeDetachSurfaceTexture: Surface detached");
    }
}

JNIEXPORT jint JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeLoadSubtitle(
    JNIEnv *env, jobject thiz, jlong playerContext, jstring path) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeLoadSubtitle: PlayerContext is NULL");
        return -1;
    }
    
    if (!path) {
        ALOGE("❌ nativeLoadSubtitle: path is NULL");
        return -1;
    }
    
    const char *path_str = (*env)->GetStringUTFChars(env, path, NULL);
    if (!path_str) {
        ALOGE("❌ nativeLoadSubtitle: Failed to get path string");
        return -1;
    }
    
    // Определяем тип файла по расширению
    int ret = -1;
    const char *ext = strrchr(path_str, '.');
    if (ext) {
        if (strcmp(ext, ".srt") == 0) {
            ret = subtitle_manager_parse_srt(&ctx->subtitles, path_str);
        } else if (strcmp(ext, ".ass") == 0 || strcmp(ext, ".ssa") == 0) {
            ret = subtitle_manager_parse_ass(&ctx->subtitles, path_str);
        } else {
            ALOGE("❌ nativeLoadSubtitle: Unsupported subtitle format: %s", ext);
        }
    } else {
        ALOGE("❌ nativeLoadSubtitle: No file extension found");
    }
    
    (*env)->ReleaseStringUTFChars(env, path, path_str);
    
    if (ret < 0) {
        ALOGE("❌ nativeLoadSubtitle: Failed to load subtitle: %d", ret);
        return -1;
    }
    
    ALOGI("✅ nativeLoadSubtitle: Subtitle loaded from %s", path_str);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeSetSubtitleEnabled(
    JNIEnv *env, jobject thiz, jlong playerContext, jboolean enabled) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeSetSubtitleEnabled: PlayerContext is NULL");
        return;
    }
    
    ctx->subtitles_enabled = enabled ? 1 : 0;
    // subtitle_manager не имеет функции set_enabled, флаг управляется через ctx->subtitles_enabled
    ALOGI("✅ nativeSetSubtitleEnabled: Subtitles %s", enabled ? "enabled" : "disabled");
}

JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeClearSubtitles(
    JNIEnv *env, jobject thiz, jlong playerContext) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeClearSubtitles: PlayerContext is NULL");
        return;
    }
    
    subtitle_manager_clear(&ctx->subtitles);
    ALOGI("✅ nativeClearSubtitles: Subtitles cleared");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: JNI функция для получения preview кадра (RGBA8888 bitmap)
/// 
/// Preview полностью независим от PlayerContext:
/// - Не использует EGL / Surface
/// - Не использует render loop
/// - Не использует threads
/// - CPU-only декодирование
/// 
/// @param path Путь к видео файлу
/// @param target_ms Целевая позиция в миллисекундах
/// @param out_w Ширина выходного bitmap
/// @param out_h Высота выходного bitmap
/// @return jbyteArray с RGBA8888 данными (размер = out_w * out_h * 4) или NULL при ошибке
JNIEXPORT jbyteArray JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeGetPreviewFrame(
    JNIEnv *env,
    jobject thiz,
    jstring path,
    jlong target_ms,
    jint out_w,
    jint out_h
) {
    if (!path || out_w <= 0 || out_h <= 0) {
        ALOGE("❌ nativeGetPreviewFrame: Invalid arguments");
        return NULL;
    }
    
    const char *path_str = (*env)->GetStringUTFChars(env, path, NULL);
    if (!path_str) {
        ALOGE("❌ nativeGetPreviewFrame: Failed to get path string");
        return NULL;
    }
    
    int buffer_size = out_w * out_h * 4; // RGBA8888
    uint8_t *buffer = (uint8_t *)malloc(buffer_size);
    if (!buffer) {
        ALOGE("❌ nativeGetPreviewFrame: Failed to allocate buffer");
        (*env)->ReleaseStringUTFChars(env, path, path_str);
        return NULL;
    }
    
    // Вызываем нативную функцию preview
    int ret = native_preview_get_frame(
        path_str,
        (int64_t)target_ms,
        (int)out_w,
        (int)out_h,
        buffer,
        buffer_size
    );
    
    (*env)->ReleaseStringUTFChars(env, path, path_str);
    
    if (ret < 0) {
        ALOGE("❌ nativeGetPreviewFrame: Preview failed: %d", ret);
        free(buffer);
        return NULL;
    }
    
    // Создаём jbyteArray и копируем данные
    jbyteArray result = (*env)->NewByteArray(env, buffer_size);
    if (!result) {
        ALOGE("❌ nativeGetPreviewFrame: Failed to create byte array");
        free(buffer);
        return NULL;
    }
    
    (*env)->SetByteArrayRegion(env, result, 0, buffer_size, (jbyte *)buffer);
    free(buffer);
    
    ALOGI("✅ nativeGetPreviewFrame: Preview frame extracted successfully (%dx%d)", 
          out_w, out_h);
    
    return result;
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.4: Native API для background playback
JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeOnAppBackground(
    JNIEnv *env,
    jobject thiz,
    jlong playerContext
) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeOnAppBackground: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 nativeOnAppBackground: PlayerContext=%p", (void *)ctx);
    
    extern void native_on_background(PlayerContext *ctx);
    native_on_background(ctx);
    
    ALOGI("✅ nativeOnAppBackground: Background mode activated");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.4: Native API для foreground playback
JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeOnAppForeground(
    JNIEnv *env,
    jobject thiz,
    jlong playerContext
) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeOnAppForeground: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 nativeOnAppForeground: PlayerContext=%p", (void *)ctx);
    
    extern void native_on_foreground(PlayerContext *ctx);
    native_on_foreground(ctx);
    
    ALOGI("✅ nativeOnAppForeground: Foreground mode activated");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.2: Native API для frame stepping
JNIEXPORT void JNICALL
Java_com_media_video_music_player_NativeFfmpegPlayerPlugin_nativeStepFrame(
    JNIEnv *env,
    jobject thiz,
    jlong playerContext,
    jint direction
) {
    PlayerContext *ctx = (PlayerContext *)playerContext;
    if (!ctx) {
        ALOGE("❌ nativeStepFrame: PlayerContext is NULL");
        return;
    }
    
    ALOGI("🔄 nativeStepFrame: PlayerContext=%p, direction=%d", (void *)ctx, direction);
    
    extern void native_step_frame(PlayerContext *ctx, int direction);
    native_step_frame(ctx, direction);
    
    ALOGI("✅ nativeStepFrame: Frame step completed");
}

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.5: Эмит диагностического события
/// Эмитится для Flutter HUD с информацией о AVSYNC состоянии
/// @param type Тип события (например, "avsync")
/// @param key Ключ (например, "master", "audio_stalled")
/// @param value Значение (например, "audio", "1")
void native_player_emit_diagnostic_event(const char *type, const char *key, const char *value) {
    if (!g_jvm) {
        return;  // JVM не инициализирован
    }
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (!g_event_callback || !g_on_event_method) {
        pthread_mutex_unlock(&g_jni_mutex);
        return;  // Event callback не зарегистрирован
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    JNIEnv *env = NULL;
    int need_detach = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) != JNI_OK) {
            ALOGE("❌ Failed to attach thread to JVM for emit diagnostic event");
            return;
        }
        need_detach = 1;
    }
    
    // Создаём HashMap для payload
    jclass hashMapClass = (*env)->FindClass(env, "java/util/HashMap");
    if (!hashMapClass) {
        ALOGE("❌ Failed to find HashMap class");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jmethodID hashMapInit = (*env)->GetMethodID(env, hashMapClass, "<init>", "()V");
    jmethodID hashMapPut = (*env)->GetMethodID(env, hashMapClass, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    if (!hashMapInit || !hashMapPut) {
        ALOGE("❌ Failed to get HashMap methods");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    jobject payload_map = (*env)->NewObject(env, hashMapClass, hashMapInit);
    if (!payload_map) {
        ALOGE("❌ Failed to create HashMap");
        if (need_detach) {
            (*g_jvm)->DetachCurrentThread(g_jvm);
        }
        return;
    }
    
    // Добавляем type, key, value в payload
    jstring type_str = (*env)->NewStringUTF(env, type ? type : "");
    jstring key_str = (*env)->NewStringUTF(env, key ? key : "");
    jstring value_str = (*env)->NewStringUTF(env, value ? value : "");
    
    (*env)->CallObjectMethod(env, payload_map, hashMapPut, (*env)->NewStringUTF(env, "type"), type_str);
    (*env)->CallObjectMethod(env, payload_map, hashMapPut, (*env)->NewStringUTF(env, "key"), key_str);
    (*env)->CallObjectMethod(env, payload_map, hashMapPut, (*env)->NewStringUTF(env, "value"), value_str);
    
    jstring event_type = (*env)->NewStringUTF(env, "diagnostic");
    
    pthread_mutex_lock(&g_jni_mutex);
    
    if (g_event_callback && g_on_event_method) {
        (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload_map);
        
        if ((*env)->ExceptionCheck(env)) {
            ALOGE("❌ Exception in diagnostic event callback");
            (*env)->ExceptionDescribe(env);
            (*env)->ExceptionClear(env);
        }
    }
    
    pthread_mutex_unlock(&g_jni_mutex);
    
    (*env)->DeleteLocalRef(env, event_type);
    (*env)->DeleteLocalRef(env, payload_map);
    (*env)->DeleteLocalRef(env, type_str);
    (*env)->DeleteLocalRef(env, key_str);
    (*env)->DeleteLocalRef(env, value_str);
    
    if (need_detach) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
}

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.8: Эмит события frameStepped
void native_player_emit_frame_stepped_event(int64_t pts_ms) {
    if (!g_event_callback || !g_on_event_method) {
        ALOGD("⚠️ native_player_emit_frame_stepped_event: Event callback not ready");
        return;
    }
    
    JNIEnv *env = NULL;
    int attached = 0;
    
    if ((*g_jvm)->GetEnv(g_jvm, (void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
        if ((*g_jvm)->AttachCurrentThread(g_jvm, &env, NULL) == JNI_OK) {
            attached = 1;
        } else {
            ALOGE("❌ native_player_emit_frame_stepped_event: Failed to attach thread");
            return;
        }
    }
    
    if (!env) {
        ALOGE("❌ native_player_emit_frame_stepped_event: JNIEnv is NULL");
        return;
    }
    
    // Создаём payload map
    jclass mapClass = (*env)->FindClass(env, "java/util/HashMap");
    jmethodID mapInit = (*env)->GetMethodID(env, mapClass, "<init>", "()V");
    jobject payload = (*env)->NewObject(env, mapClass, mapInit);
    
    jmethodID mapPut = (*env)->GetMethodID(env, mapClass, "put", 
                                           "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    jstring key_pts = (*env)->NewStringUTF(env, "pts");
    jlong value_pts = (jlong)pts_ms;
    jclass longClass = (*env)->FindClass(env, "java/lang/Long");
    jmethodID longValueOf = (*env)->GetStaticMethodID(env, longClass, "valueOf", "(J)Ljava/lang/Long;");
    jobject pts_obj = (*env)->CallStaticObjectMethod(env, longClass, longValueOf, value_pts);
    (*env)->CallObjectMethod(env, payload, mapPut, key_pts, pts_obj);
    (*env)->DeleteLocalRef(env, key_pts);
    (*env)->DeleteLocalRef(env, pts_obj);
    
    // Вызываем callback
    jstring event_type = (*env)->NewStringUTF(env, "frameStepped");
    (*env)->CallVoidMethod(env, g_event_callback, g_on_event_method, event_type, payload);
    
    (*env)->DeleteLocalRef(env, event_type);
    (*env)->DeleteLocalRef(env, payload);
    
    if (attached) {
        (*g_jvm)->DetachCurrentThread(g_jvm);
    }
    
    ALOGI("✅ native_player_emit_frame_stepped_event: Event emitted (pts=%lld ms)", (long long)pts_ms);
}
