#include <jni.h>
#include <string.h>
#include <android/log.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define LOG_TAG "SmartFfmpegBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/**
 * Extract thumbnail from video at specified timestamp
 * JNI signature: Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_extractThumbnail
 */
JNIEXPORT jbyteArray JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_extractThumbnail(
    JNIEnv *env,
    jobject thiz,
    jstring videoPath,
    jlong timeMs,
    jint width,
    jint height
) {
    const char *path = (*env)->GetStringUTFChars(env, videoPath, NULL);
    if (path == NULL) {
        LOGE("Failed to get video path string");
        return NULL;
    }

    LOGI("Extracting thumbnail from: %s at %lld ms, size: %dx%d", path, (long long)timeMs, width, height);

    AVFormatContext *formatCtx = NULL;
    AVCodecContext *codecCtx = NULL;
    AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVFrame *frameRGBA = NULL;
    AVPacket packet;
    struct SwsContext *swsCtx = NULL;
    jbyteArray result = NULL;
    int videoStream = -1;

    // Open video file
    if (avformat_open_input(&formatCtx, path, NULL, NULL) != 0) {
        LOGE("Could not open video file: %s", path);
        goto cleanup;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatCtx, NULL) < 0) {
        LOGE("Could not find stream information");
        goto cleanup;
    }

    // Find video stream
    for (int i = 0; i < formatCtx->nb_streams; i++) {
        if (formatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStream = i;
            break;
        }
    }

    if (videoStream == -1) {
        LOGE("Could not find video stream");
        goto cleanup;
    }

    // Get codec parameters
    AVCodecParameters *codecParams = formatCtx->streams[videoStream]->codecpar;
    
    // Find decoder
    codec = avcodec_find_decoder(codecParams->codec_id);
    if (codec == NULL) {
        LOGE("Unsupported codec");
        goto cleanup;
    }

    // Allocate codec context
    codecCtx = avcodec_alloc_context3(codec);
    if (codecCtx == NULL) {
        LOGE("Could not allocate codec context");
        goto cleanup;
    }

    // Copy codec parameters to context
    if (avcodec_parameters_to_context(codecCtx, codecParams) < 0) {
        LOGE("Could not copy codec parameters");
        goto cleanup;
    }

    // Open codec
    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        LOGE("Could not open codec");
        goto cleanup;
    }

    // Allocate frames
    frame = av_frame_alloc();
    frameRGBA = av_frame_alloc();
    if (frame == NULL || frameRGBA == NULL) {
        LOGE("Could not allocate frames");
        goto cleanup;
    }

    // Determine output size
    int outWidth = width > 0 ? width : codecCtx->width;
    int outHeight = height > 0 ? height : codecCtx->height;

    // Allocate buffer for RGBA frame
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, outWidth, outHeight, 1);
    uint8_t *buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
    if (buffer == NULL) {
        LOGE("Could not allocate buffer");
        goto cleanup;
    }

    av_image_fill_arrays(frameRGBA->data, frameRGBA->linesize, buffer,
                        AV_PIX_FMT_RGBA, outWidth, outHeight, 1);

    // Initialize SWS context for software scaling
    swsCtx = sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        outWidth, outHeight, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, NULL, NULL, NULL
    );

    if (swsCtx == NULL) {
        LOGE("Could not initialize SWS context");
        av_free(buffer);
        goto cleanup;
    }

    // Seek to timestamp
    int64_t timestamp = (timeMs * formatCtx->streams[videoStream]->time_base.den) / 
                       (1000 * formatCtx->streams[videoStream]->time_base.num);
    
    if (av_seek_frame(formatCtx, videoStream, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        LOGE("Could not seek to timestamp");
    }

    avcodec_flush_buffers(codecCtx);

    // Read frames until we find the one we want
    int frameFinished = 0;
    while (av_read_frame(formatCtx, &packet) >= 0) {
        if (packet.stream_index == videoStream) {
            // Decode video frame
            int ret = avcodec_send_packet(codecCtx, &packet);
            if (ret < 0) {
                LOGE("Error sending packet for decoding");
                av_packet_unref(&packet);
                continue;
            }

            ret = avcodec_receive_frame(codecCtx, frame);
            if (ret == 0) {
                frameFinished = 1;
                av_packet_unref(&packet);
                break;
            } else if (ret == AVERROR(EAGAIN)) {
                // Need more packets
                av_packet_unref(&packet);
                continue;
            } else {
                LOGE("Error receiving frame");
                av_packet_unref(&packet);
                break;
            }
        }
        av_packet_unref(&packet);
    }

    if (frameFinished) {
        // Convert to RGBA
        sws_scale(swsCtx, (uint8_t const *const *)frame->data,
                 frame->linesize, 0, codecCtx->height,
                 frameRGBA->data, frameRGBA->linesize);

        // Create Java byte array
        result = (*env)->NewByteArray(env, numBytes);
        if (result != NULL) {
            (*env)->SetByteArrayRegion(env, result, 0, numBytes, (jbyte *)buffer);
            LOGI("Successfully extracted thumbnail: %d bytes", numBytes);
        } else {
            LOGE("Could not create byte array");
        }
    } else {
        LOGE("Could not decode frame");
    }

    av_free(buffer);
    sws_freeContext(swsCtx);

cleanup:
    if (frame) av_frame_free(&frame);
    if (frameRGBA) av_frame_free(&frameRGBA);
    if (codecCtx) avcodec_free_context(&codecCtx);
    if (formatCtx) avformat_close_input(&formatCtx);
    (*env)->ReleaseStringUTFChars(env, videoPath, path);

    return result;
}

/**
 * Get video duration in milliseconds
 * JNI signature: Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoDuration
 */
JNIEXPORT jlong JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoDuration(
    JNIEnv *env,
    jobject thiz,
    jstring videoPath
) {
    const char *path = (*env)->GetStringUTFChars(env, videoPath, NULL);
    if (path == NULL) {
        LOGE("Failed to get video path string");
        return -1;
    }

    LOGI("Getting duration for: %s", path);

    AVFormatContext *formatCtx = NULL;
    jlong duration = -1;

    // Open video file
    if (avformat_open_input(&formatCtx, path, NULL, NULL) != 0) {
        LOGE("Could not open video file: %s", path);
        goto cleanup;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatCtx, NULL) < 0) {
        LOGE("Could not find stream information");
        goto cleanup;
    }

    // Get duration in milliseconds
    if (formatCtx->duration != AV_NOPTS_VALUE) {
        duration = (jlong)(formatCtx->duration / (AV_TIME_BASE / 1000));
        LOGI("Duration: %lld ms", (long long)duration);
    } else {
        LOGE("Duration not available");
    }

cleanup:
    if (formatCtx) avformat_close_input(&formatCtx);
    (*env)->ReleaseStringUTFChars(env, videoPath, path);

    return duration;
}

/**
 * Get video metadata (extended version with more fields)
 * JNI signature: Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadata
 */
JNIEXPORT jobject JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadata(
    JNIEnv *env,
    jobject thiz,
    jstring videoPath
) {
    const char *path = (*env)->GetStringUTFChars(env, videoPath, NULL);
    if (path == NULL) {
        LOGE("Failed to get video path string");
        return NULL;
    }

    LOGI("Getting metadata for: %s", path);

    AVFormatContext *formatCtx = NULL;
    jobject result = NULL;
    int videoStream = -1;
    int audioStream = -1;

    // Open video file
    if (avformat_open_input(&formatCtx, path, NULL, NULL) != 0) {
        LOGE("Could not open video file: %s", path);
        goto cleanup;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatCtx, NULL) < 0) {
        LOGE("Could not find stream information");
        goto cleanup;
    }

    // Find video and audio streams
    int subtitleCount = 0;
    for (int i = 0; i < formatCtx->nb_streams; i++) {
        enum AVMediaType type = formatCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStream == -1) {
            videoStream = i;
        } else if (type == AVMEDIA_TYPE_AUDIO && audioStream == -1) {
            audioStream = i;
        } else if (type == AVMEDIA_TYPE_SUBTITLE) {
            subtitleCount++;
        }
    }

    if (videoStream == -1) {
        LOGE("Could not find video stream");
        goto cleanup;
    }

    // Create HashMap
    jclass hashMapClass = (*env)->FindClass(env, "java/util/HashMap");
    jmethodID hashMapInit = (*env)->GetMethodID(env, hashMapClass, "<init>", "()V");
    jmethodID hashMapPut = (*env)->GetMethodID(env, hashMapClass, "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    result = (*env)->NewObject(env, hashMapClass, hashMapInit);

    // Helper classes
    jclass integerClass = (*env)->FindClass(env, "java/lang/Integer");
    jmethodID integerInit = (*env)->GetMethodID(env, integerClass, "<init>", "(I)V");
    jclass longClass = (*env)->FindClass(env, "java/lang/Long");
    jmethodID longInit = (*env)->GetMethodID(env, longClass, "<init>", "(J)V");
    jclass booleanClass = (*env)->FindClass(env, "java/lang/Boolean");
    jmethodID booleanInit = (*env)->GetMethodID(env, booleanClass, "<init>", "(Z)V");
    jclass doubleClass = (*env)->FindClass(env, "java/lang/Double");
    jmethodID doubleInit = (*env)->GetMethodID(env, doubleClass, "<init>", "(D)V");

    // Get video codec parameters
    AVCodecParameters *codecParams = formatCtx->streams[videoStream]->codecpar;
    AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
    AVStream *stream = formatCtx->streams[videoStream];

    // Add width
    jobject widthObj = (*env)->NewObject(env, integerClass, integerInit, codecParams->width);
    (*env)->CallObjectMethod(env, result, hashMapPut,
        (*env)->NewStringUTF(env, "width"), widthObj);

    // Add height
    jobject heightObj = (*env)->NewObject(env, integerClass, integerInit, codecParams->height);
    (*env)->CallObjectMethod(env, result, hashMapPut,
        (*env)->NewStringUTF(env, "height"), heightObj);

    // Add duration
    if (formatCtx->duration != AV_NOPTS_VALUE) {
        jlong duration = (jlong)(formatCtx->duration / (AV_TIME_BASE / 1000));
        jobject durationObj = (*env)->NewObject(env, longClass, longInit, duration);
        (*env)->CallObjectMethod(env, result, hashMapPut,
            (*env)->NewStringUTF(env, "duration"), durationObj);
    }

    // Add codec name
    if (codec) {
        (*env)->CallObjectMethod(env, result, hashMapPut,
            (*env)->NewStringUTF(env, "codec"),
            (*env)->NewStringUTF(env, codec->name));
    }

    // Add bitrate
    if (codecParams->bit_rate > 0) {
        jobject bitrateObj = (*env)->NewObject(env, longClass, longInit, (jlong)codecParams->bit_rate);
        (*env)->CallObjectMethod(env, result, hashMapPut,
            (*env)->NewStringUTF(env, "bitrate"), bitrateObj);
    }

    // Add FPS
    AVRational frameRate = av_guess_frame_rate(formatCtx, stream, NULL);
    if (frameRate.num > 0 && frameRate.den > 0) {
        double fps = (double)frameRate.num / (double)frameRate.den;
        jobject fpsObj = (*env)->NewObject(env, doubleClass, doubleInit, fps);
        (*env)->CallObjectMethod(env, result, hashMapPut,
            (*env)->NewStringUTF(env, "fps"), fpsObj);
    }

    // Add rotation
    AVDictionaryEntry *rotate_tag = av_dict_get(stream->metadata, "rotate", NULL, 0);
    int rotation = 0;
    if (rotate_tag && rotate_tag->value) {
        rotation = atoi(rotate_tag->value);
    }
    jobject rotationObj = (*env)->NewObject(env, integerClass, integerInit, rotation);
    (*env)->CallObjectMethod(env, result, hashMapPut,
        (*env)->NewStringUTF(env, "rotation"), rotationObj);

    // Add container format
    if (formatCtx->iformat && formatCtx->iformat->name) {
        (*env)->CallObjectMethod(env, result, hashMapPut,
            (*env)->NewStringUTF(env, "container"),
            (*env)->NewStringUTF(env, formatCtx->iformat->name));
    }

    // Add stream count
    jobject streamCountObj = (*env)->NewObject(env, integerClass, integerInit, formatCtx->nb_streams);
    (*env)->CallObjectMethod(env, result, hashMapPut,
        (*env)->NewStringUTF(env, "streamCount"), streamCountObj);

    // Add hasAudio
    jobject hasAudioObj = (*env)->NewObject(env, booleanClass, booleanInit, audioStream != -1);
    (*env)->CallObjectMethod(env, result, hashMapPut,
        (*env)->NewStringUTF(env, "hasAudio"), hasAudioObj);

    // Add hasSubtitles
    jobject hasSubtitlesObj = (*env)->NewObject(env, booleanClass, booleanInit, subtitleCount > 0);
    (*env)->CallObjectMethod(env, result, hashMapPut,
        (*env)->NewStringUTF(env, "hasSubtitles"), hasSubtitlesObj);

    // Add audio metadata if audio stream exists
    if (audioStream != -1) {
        AVCodecParameters *audioCodecParams = formatCtx->streams[audioStream]->codecpar;
        AVCodec *audioCodec = avcodec_find_decoder(audioCodecParams->codec_id);
        
        // Audio codec
        if (audioCodec) {
            (*env)->CallObjectMethod(env, result, hashMapPut,
                (*env)->NewStringUTF(env, "audioCodec"),
                (*env)->NewStringUTF(env, audioCodec->name));
        }
        
        // Sample rate
        if (audioCodecParams->sample_rate > 0) {
            jobject sampleRateObj = (*env)->NewObject(env, integerClass, integerInit, audioCodecParams->sample_rate);
            (*env)->CallObjectMethod(env, result, hashMapPut,
                (*env)->NewStringUTF(env, "sampleRate"), sampleRateObj);
        }
        
        // Channels
        if (audioCodecParams->channels > 0) {
            jobject channelsObj = (*env)->NewObject(env, integerClass, integerInit, audioCodecParams->channels);
            (*env)->CallObjectMethod(env, result, hashMapPut,
                (*env)->NewStringUTF(env, "channels"), channelsObj);
        }
    }

    LOGI("Metadata extracted successfully");

cleanup:
    if (formatCtx) avformat_close_input(&formatCtx);
    (*env)->ReleaseStringUTFChars(env, videoPath, path);

    return result;
}

/**
 * Get FFmpeg version
 * JNI signature: Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getFFmpegVersion
 */
JNIEXPORT jstring JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getFFmpegVersion(
    JNIEnv *env,
    jobject thiz
) {
    const char *version = av_version_info();
    LOGI("FFmpeg version: %s", version);
    return (*env)->NewStringUTF(env, version);
}

/**
 * Get video metadata as JSON string (with safe-mode error handling)
 * JNI signature: Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadataJson
 */
JNIEXPORT jstring JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadataJson(
    JNIEnv *env,
    jobject thiz,
    jstring videoPath
) {
    const char *path = (*env)->GetStringUTFChars(env, videoPath, NULL);
    if (path == NULL) {
        LOGE("Failed to get video path string");
        return (*env)->NewStringUTF(env, "{\"success\":false,\"error\":\"Invalid path\"}");
    }

    LOGI("Getting JSON metadata for: %s", path);

    AVFormatContext *formatCtx = NULL;
    char jsonBuffer[8192];
    int videoStream = -1;
    int audioStream = -1;
    int subtitleCount = 0;

    // Open video file with error handling
    int ret = avformat_open_input(&formatCtx, path, NULL, NULL);
    if (ret != 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOGE("Could not open video file: %s, error: %s", path, errBuf);
        snprintf(jsonBuffer, sizeof(jsonBuffer),
            "{\"success\":false,\"error\":\"Could not open file: %s\"}", errBuf);
        (*env)->ReleaseStringUTFChars(env, videoPath, path);
        return (*env)->NewStringUTF(env, jsonBuffer);
    }

    // Retrieve stream information with error handling
    ret = avformat_find_stream_info(formatCtx, NULL);
    if (ret < 0) {
        char errBuf[128];
        av_strerror(ret, errBuf, sizeof(errBuf));
        LOGE("Could not find stream information: %s", errBuf);
        snprintf(jsonBuffer, sizeof(jsonBuffer),
            "{\"success\":false,\"error\":\"Could not find stream info: %s\"}", errBuf);
        avformat_close_input(&formatCtx);
        (*env)->ReleaseStringUTFChars(env, videoPath, path);
        return (*env)->NewStringUTF(env, jsonBuffer);
    }

    // Find streams
    for (int i = 0; i < formatCtx->nb_streams; i++) {
        enum AVMediaType type = formatCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStream == -1) {
            videoStream = i;
        } else if (type == AVMEDIA_TYPE_AUDIO && audioStream == -1) {
            audioStream = i;
        } else if (type == AVMEDIA_TYPE_SUBTITLE) {
            subtitleCount++;
        }
    }

    if (videoStream == -1) {
        LOGE("Could not find video stream");
        snprintf(jsonBuffer, sizeof(jsonBuffer),
            "{\"success\":false,\"error\":\"No video stream found\"}");
        avformat_close_input(&formatCtx);
        (*env)->ReleaseStringUTFChars(env, videoPath, path);
        return (*env)->NewStringUTF(env, jsonBuffer);
    }

    // Build JSON manually (safe and simple)
    char *ptr = jsonBuffer;
    int remaining = sizeof(jsonBuffer);
    int written;

    // Start JSON
    written = snprintf(ptr, remaining, "{\"success\":true,\"data\":{");
    ptr += written; remaining -= written;

    // Get video stream info
    AVCodecParameters *codecParams = formatCtx->streams[videoStream]->codecpar;
    AVCodec *codec = avcodec_find_decoder(codecParams->codec_id);
    AVStream *stream = formatCtx->streams[videoStream];

    // Width and height
    written = snprintf(ptr, remaining, "\"width\":%d,\"height\":%d,",
        codecParams->width, codecParams->height);
    ptr += written; remaining -= written;

    // Duration
    if (formatCtx->duration != AV_NOPTS_VALUE) {
        jlong duration = (jlong)(formatCtx->duration / (AV_TIME_BASE / 1000));
        written = snprintf(ptr, remaining, "\"duration\":%lld,", (long long)duration);
        ptr += written; remaining -= written;
    }

    // Codec
    if (codec && codec->name) {
        written = snprintf(ptr, remaining, "\"codec\":\"%s\",", codec->name);
        ptr += written; remaining -= written;
    }

    // Bitrate
    if (codecParams->bit_rate > 0) {
        written = snprintf(ptr, remaining, "\"bitrate\":%lld,", (long long)codecParams->bit_rate);
        ptr += written; remaining -= written;
    }

    // FPS
    AVRational frameRate = av_guess_frame_rate(formatCtx, stream, NULL);
    if (frameRate.num > 0 && frameRate.den > 0) {
        double fps = (double)frameRate.num / (double)frameRate.den;
        written = snprintf(ptr, remaining, "\"fps\":%.2f,", fps);
        ptr += written; remaining -= written;
    }

    // Rotation
    AVDictionaryEntry *rotate_tag = av_dict_get(stream->metadata, "rotate", NULL, 0);
    int rotation = 0;
    if (rotate_tag && rotate_tag->value) {
        rotation = atoi(rotate_tag->value);
    }
    written = snprintf(ptr, remaining, "\"rotation\":%d,", rotation);
    ptr += written; remaining -= written;

    // Container
    if (formatCtx->iformat && formatCtx->iformat->name) {
        written = snprintf(ptr, remaining, "\"container\":\"%s\",", formatCtx->iformat->name);
        ptr += written; remaining -= written;
    }

    // Stream count
    written = snprintf(ptr, remaining, "\"streamCount\":%d,", formatCtx->nb_streams);
    ptr += written; remaining -= written;

    // Has audio
    written = snprintf(ptr, remaining, "\"hasAudio\":%s,", audioStream != -1 ? "true" : "false");
    ptr += written; remaining -= written;

    // Has subtitles
    written = snprintf(ptr, remaining, "\"hasSubtitles\":%s", subtitleCount > 0 ? "true" : "false");
    ptr += written; remaining -= written;

    // Audio metadata if exists
    if (audioStream != -1) {
        AVCodecParameters *audioCodecParams = formatCtx->streams[audioStream]->codecpar;
        AVCodec *audioCodec = avcodec_find_decoder(audioCodecParams->codec_id);
        
        written = snprintf(ptr, remaining, ",");
        ptr += written; remaining -= written;
        
        if (audioCodec && audioCodec->name) {
            written = snprintf(ptr, remaining, "\"audioCodec\":\"%s\",", audioCodec->name);
            ptr += written; remaining -= written;
        }
        
        if (audioCodecParams->sample_rate > 0) {
            written = snprintf(ptr, remaining, "\"sampleRate\":%d,", audioCodecParams->sample_rate);
            ptr += written; remaining -= written;
        }
        
        if (audioCodecParams->channels > 0) {
            written = snprintf(ptr, remaining, "\"channels\":%d", audioCodecParams->channels);
            ptr += written; remaining -= written;
        }
    }

    // Close JSON
    written = snprintf(ptr, remaining, "}}");
    ptr += written; remaining -= written;

    LOGI("JSON metadata created successfully");

    avformat_close_input(&formatCtx);
    (*env)->ReleaseStringUTFChars(env, videoPath, path);

    return (*env)->NewStringUTF(env, jsonBuffer);
}
