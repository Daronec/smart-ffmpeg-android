/// Шаг 29: MediaCodec Video Decode (Surface / Hybrid Mode)

#include "video_backend_mediacodec.h"
#include "audio_renderer.h"  // Для полного определения AudioState
#include "packet_queue.h"
#include "clock.h"
#include <android/log.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"

#define LOG_TAG "MediaCodecBackend"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// === JNI Helpers ===

/// Получить MIME type из codec ID (Шаг 29.2)
static const char *get_mime_from_codec_id(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return "video/avc";
        case AV_CODEC_ID_HEVC:
            return "video/hevc";
        case AV_CODEC_ID_VP9:
            return "video/x-vnd.on2.vp9";
        default:
            return NULL;
    }
}

/// Поток декодирования MediaCodec (Шаг 29.5, 29.6)
static void *mediacodec_decode_thread(void *arg) {
    VideoBackendMediaCodec *backend = (VideoBackendMediaCodec *)arg;
    JNIEnv *env = NULL;
    
    // Attach thread к JVM
    if ((*backend->jvm)->AttachCurrentThread(backend->jvm, &env, NULL) != JNI_OK) {
        ALOGE("Failed to attach thread to JVM");
        return NULL;
    }
    
    // Получаем классы и методы (кэшируем при первом вызове)
    jclass mc_cls = (*env)->FindClass(env, "android/media/MediaCodec");
    jclass mf_cls = (*env)->FindClass(env, "android/media/MediaFormat");
    jclass bi_cls = (*env)->FindClass(env, "android/media/MediaCodec$BufferInfo");
    
    if (!mc_cls || !mf_cls || !bi_cls) {
        ALOGE("Failed to find MediaCodec classes");
        (*backend->jvm)->DetachCurrentThread(backend->jvm);
        return NULL;
    }
    
    // Методы MediaCodec
    jmethodID dequeue_input_mid = (*env)->GetMethodID(env, mc_cls, "dequeueInputBuffer", "(J)I");
    jmethodID get_input_buffer_mid = (*env)->GetMethodID(env, mc_cls, "getInputBuffer", "(I)Ljava/nio/ByteBuffer;");
    jmethodID queue_input_buffer_mid = (*env)->GetMethodID(env, mc_cls, "queueInputBuffer", "(IIIJI)V");
    jmethodID dequeue_output_mid = (*env)->GetMethodID(env, mc_cls, "dequeueOutputBuffer", "(Landroid/media/MediaCodec$BufferInfo;J)I");
    jmethodID release_output_buffer_mid = (*env)->GetMethodID(env, mc_cls, "releaseOutputBuffer", "(IZ)V");
    
    // Создаём BufferInfo объект
    jmethodID bi_ctor = (*env)->GetMethodID(env, bi_cls, "<init>", "()V");
    jobject buffer_info = (*env)->NewObject(env, bi_cls, bi_ctor);
    
    ALOGI("MediaCodec decode thread started");
    
    while (!backend->abort) {
        if (backend->paused) {
            // Шаг 29.8: Pause (не останавливаем MediaCodec)
            usleep(10000); // 10ms
            continue;
        }
        
        // Шаг 29.5: Feed packets в MediaCodec
        AVPacket pkt;
        if (packet_queue_get(backend->packet_queue, &pkt, false) > 0) {
            // Dequeue input buffer
            jint input_index = (*env)->CallIntMethod(env, backend->media_codec, dequeue_input_mid, (jlong)10000); // 10ms timeout
            
            if (input_index >= 0) {
                // Получаем ByteBuffer
                jobject input_buffer = (*env)->CallObjectMethod(env, backend->media_codec, get_input_buffer_mid, input_index);
                
                if (input_buffer) {
                    // Копируем данные пакета
                    void *buffer_ptr = (*env)->GetDirectBufferAddress(env, input_buffer);
                    jlong buffer_capacity = (*env)->GetDirectBufferCapacity(env, input_buffer);
                    
                    if (buffer_ptr && buffer_capacity >= pkt.size) {
                        memcpy(buffer_ptr, pkt.data, pkt.size);
                        
                        // Вычисляем PTS в микросекундах
                        int64_t pts_us = pkt.pts == AV_NOPTS_VALUE ? 0 : (int64_t)(pkt.pts * 1000000.0 / 90000.0); // Предполагаем 90kHz timebase
                        
                        // Queue input buffer (Шаг 29.5)
                        (*env)->CallVoidMethod(env, backend->media_codec, queue_input_buffer_mid,
                                             input_index, 0, (jint)pkt.size, (jlong)pts_us, 0);
                    }
                    
                    (*env)->DeleteLocalRef(env, input_buffer);
                }
                
                av_packet_unref(&pkt);
            } else {
                // Input buffer не доступен - освобождаем пакет
                // Следующий пакет будет взят из очереди в следующей итерации
                av_packet_unref(&pkt);
            }
        }
        
        // Шаг 29.6: Drain output (Surface sync)
        jint output_index = (*env)->CallIntMethod(env, backend->media_codec, dequeue_output_mid, buffer_info, (jlong)10000);
        
        if (output_index >= 0) {
            // Получаем PTS из BufferInfo
            jfieldID pts_field = (*env)->GetFieldID(env, bi_cls, "presentationTimeUs", "J");
            jlong pts_us = (*env)->GetLongField(env, buffer_info, pts_field);
            double video_pts = pts_us / 1e6;
            
            // Шаг 29.7: A/V Sync (Audio master)
            if (backend->audio_state && clock_is_active(&((AudioState *)backend->audio_state)->clock)) {
                double audio_clock = clock_get(&((AudioState *)backend->audio_state)->clock);
                double diff = video_pts - audio_clock;
                
                if (diff > 0.04) {
                    // Video ahead → sleep
                    usleep((int)(diff * 1e6));
                } else if (diff < -0.1) {
                    // Video late → drop frame
                    (*env)->CallVoidMethod(env, backend->media_codec, release_output_buffer_mid, output_index, JNI_FALSE);
                    continue;
                }
            }
            
            // Обновляем video clock
            clock_set(&backend->video_clock, video_pts);
            
            // Release output buffer с render=true (Шаг 29.6)
            (*env)->CallVoidMethod(env, backend->media_codec, release_output_buffer_mid, output_index, JNI_TRUE);
        } else if (output_index == -2) {
            // INFO_OUTPUT_FORMAT_CHANGED - игнорируем
        } else if (output_index == -3) {
            // INFO_OUTPUT_BUFFERS_CHANGED - игнорируем
        }
    }
    
    // Cleanup
    (*env)->DeleteLocalRef(env, buffer_info);
    (*env)->DeleteLocalRef(env, mc_cls);
    (*env)->DeleteLocalRef(env, mf_cls);
    (*env)->DeleteLocalRef(env, bi_cls);
    
    (*backend->jvm)->DetachCurrentThread(backend->jvm);
    ALOGI("MediaCodec decode thread stopped");
    
    return NULL;
}

int video_backend_mediacodec_init(VideoBackendMediaCodec *backend,
                                  JavaVM *jvm,
                                  int64_t texture_id,
                                  int codec_id,
                                  int width,
                                  int height,
                                  PacketQueue *packet_queue,
                                  struct AudioState *audio_state) {
    if (!backend || !jvm || !packet_queue) {
        ALOGE("Invalid parameters for video_backend_mediacodec_init");
        return -1;
    }
    
    memset(backend, 0, sizeof(VideoBackendMediaCodec));
    
    backend->jvm = jvm;
    backend->texture_id = texture_id;
    backend->codec_id = codec_id;
    backend->width = width;
    backend->height = height;
    backend->packet_queue = packet_queue;
    backend->audio_state = (struct AudioState *)audio_state;
    
    // Получаем MIME type (Шаг 29.2)
    const char *mime = get_mime_from_codec_id(codec_id);
    if (!mime) {
        ALOGE("Unsupported codec ID: %d", codec_id);
        return -1;
    }
    strncpy(backend->mime, mime, sizeof(backend->mime) - 1);
    
    JNIEnv *env = NULL;
    if ((*jvm)->GetEnv(jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*jvm)->AttachCurrentThread(jvm, &env, NULL);
    }
    
    if (!env) {
        ALOGE("Failed to get JNIEnv");
        return -1;
    }
    
    // TODO: Создать MediaCodec через JNI (Шаг 29.2)
    // Это должно быть сделано из Java/Kotlin side через MethodChannel
    // Здесь мы только получаем уже созданный объект
    
    // Инициализируем clock
    clock_init(&backend->video_clock);
    
    backend->initialized = true;
    ALOGI("MediaCodec backend initialized (%s, %dx%d)", mime, width, height);
    
    return 0;
}

int video_backend_mediacodec_start(VideoBackendMediaCodec *backend) {
    if (!backend->initialized) {
        return -1;
    }
    
    backend->abort = 0;
    backend->paused = false;
    
    // Запускаем decode thread
    if (pthread_create(&backend->decode_thread, NULL, mediacodec_decode_thread, backend) != 0) {
        ALOGE("Failed to create MediaCodec decode thread");
        return -1;
    }
    
    ALOGI("MediaCodec backend started");
    return 0;
}

void video_backend_mediacodec_pause(VideoBackendMediaCodec *backend) {
    if (!backend->initialized) {
        return;
    }
    
    backend->paused = true;
    ALOGD("MediaCodec backend paused");
}

void video_backend_mediacodec_resume(VideoBackendMediaCodec *backend) {
    if (!backend->initialized) {
        return;
    }
    
    backend->paused = false;
    ALOGD("MediaCodec backend resumed");
}

int video_backend_mediacodec_seek(VideoBackendMediaCodec *backend, double pts) {
    if (!backend->initialized) {
        return -1;
    }
    
    // Шаг 29.9: Seek / Flush
    video_backend_mediacodec_flush(backend);
    
    // Сбрасываем clock
    clock_reset(&backend->video_clock, pts);
    
    ALOGI("MediaCodec backend seeked to %.3f", pts);
    return 0;
}

void video_backend_mediacodec_stop(VideoBackendMediaCodec *backend) {
    if (!backend->initialized) {
        return;
    }
    
    backend->abort = 1;
    packet_queue_abort(backend->packet_queue);
    
    if (backend->decode_thread) {
        pthread_join(backend->decode_thread, NULL);
    }
    
    ALOGI("MediaCodec backend stopped");
}

void video_backend_mediacodec_flush(VideoBackendMediaCodec *backend) {
    if (!backend->initialized) {
        return;
    }
    
    // Шаг 29.9: Flush MediaCodec
    JNIEnv *env = NULL;
    if ((*backend->jvm)->GetEnv(backend->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*backend->jvm)->AttachCurrentThread(backend->jvm, &env, NULL);
    }
    
    if (env && backend->media_codec) {
        jclass mc_cls = (*env)->GetObjectClass(env, backend->media_codec);
        jmethodID flush_mid = (*env)->GetMethodID(env, mc_cls, "flush", "()V");
        if (flush_mid) {
            (*env)->CallVoidMethod(env, backend->media_codec, flush_mid);
        }
        (*env)->DeleteLocalRef(env, mc_cls);
    }
    
    // Очищаем очередь пакетов
    packet_queue_flush(backend->packet_queue);
    
    ALOGD("MediaCodec backend flushed");
}

void video_backend_mediacodec_release(VideoBackendMediaCodec *backend) {
    if (!backend) {
        return;
    }
    
    video_backend_mediacodec_stop(backend);
    
    // Освобождаем MediaCodec через JNI
    JNIEnv *env = NULL;
    if ((*backend->jvm)->GetEnv(backend->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        (*backend->jvm)->AttachCurrentThread(backend->jvm, &env, NULL);
    }
    
    if (env) {
        if (backend->media_codec) {
            (*env)->DeleteGlobalRef(env, backend->media_codec);
        }
        if (backend->surface) {
            (*env)->DeleteGlobalRef(env, backend->surface);
        }
        if (backend->surface_texture) {
            (*env)->DeleteGlobalRef(env, backend->surface_texture);
        }
    }
    
    memset(backend, 0, sizeof(VideoBackendMediaCodec));
    ALOGI("MediaCodec backend released");
}

bool video_backend_mediacodec_is_initialized(VideoBackendMediaCodec *backend) {
    return backend && backend->initialized;
}

double video_backend_mediacodec_get_clock(VideoBackendMediaCodec *backend) {
    if (!backend || !backend->initialized) {
        return 0.0;
    }
    return clock_get(&backend->video_clock);
}

