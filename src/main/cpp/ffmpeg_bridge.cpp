#include <jni.h>
#include <string>
#include <android/log.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#define LOG_TAG "SmartFFmpeg"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_extractThumbnail(
        JNIEnv* env,
        jclass clazz,
        jstring videoPath,
        jlong timeMs,
        jint width,
        jint height) {

    const char* path = env->GetStringUTFChars(videoPath, nullptr);
    LOGI("Extracting thumbnail from: %s at %lld ms", path, (long long)timeMs);

    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* frameRGBA = nullptr;
    AVPacket* packet = nullptr;
    struct SwsContext* swsContext = nullptr;
    jbyteArray result = nullptr;

    // Open video file
    if (avformat_open_input(&formatContext, path, nullptr, nullptr) != 0) {
        LOGE("Could not open video file");
        goto cleanup;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        LOGE("Could not find stream information");
        goto cleanup;
    }

    // Find video stream
    int videoStreamIndex = -1;
    for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = i;
            break;
        }
    }

    if (videoStreamIndex == -1) {
        LOGE("Could not find video stream");
        goto cleanup;
    }

    // Get codec parameters
    AVCodecParameters* codecParams = formatContext->streams[videoStreamIndex]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        LOGE("Unsupported codec");
        goto cleanup;
    }

    // Allocate codec context
    codecContext = avcodec_alloc_context3(codec);
    if (!codecContext) {
        LOGE("Could not allocate codec context");
        goto cleanup;
    }

    if (avcodec_parameters_to_context(codecContext, codecParams) < 0) {
        LOGE("Could not copy codec parameters");
        goto cleanup;
    }

    // Open codec
    if (avcodec_open2(codecContext, codec, nullptr) < 0) {
        LOGE("Could not open codec");
        goto cleanup;
    }

    // Seek to timestamp
    int64_t timestamp = (timeMs * AV_TIME_BASE) / 1000;
    if (av_seek_frame(formatContext, -1, timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
        LOGE("Could not seek to timestamp");
    }

    // Allocate frames
    frame = av_frame_alloc();
    frameRGBA = av_frame_alloc();
    packet = av_packet_alloc();

    if (!frame || !frameRGBA || !packet) {
        LOGE("Could not allocate frames");
        goto cleanup;
    }

    // Calculate output dimensions
    int outWidth = width > 0 ? width : codecContext->width;
    int outHeight = height > 0 ? height : codecContext->height;

    // Allocate buffer for RGBA frame
    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, outWidth, outHeight, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));

    av_image_fill_arrays(frameRGBA->data, frameRGBA->linesize, buffer,
                         AV_PIX_FMT_RGBA, outWidth, outHeight, 1);

    // Initialize SWS context for color conversion
    swsContext = sws_getContext(
            codecContext->width, codecContext->height, codecContext->pix_fmt,
            outWidth, outHeight, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);

    if (!swsContext) {
        LOGE("Could not initialize SWS context");
        av_free(buffer);
        goto cleanup;
    }

    // Read frames
    bool frameFound = false;
    while (av_read_frame(formatContext, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex) {
            if (avcodec_send_packet(codecContext, packet) == 0) {
                if (avcodec_receive_frame(codecContext, frame) == 0) {
                    // Convert to RGBA
                    sws_scale(swsContext,
                              frame->data, frame->linesize, 0, codecContext->height,
                              frameRGBA->data, frameRGBA->linesize);

                    // Create Java byte array
                    result = env->NewByteArray(numBytes);
                    env->SetByteArrayRegion(result, 0, numBytes, (jbyte*)buffer);

                    frameFound = true;
                    av_packet_unref(packet);
                    break;
                }
            }
        }
        av_packet_unref(packet);
    }

    if (!frameFound) {
        LOGE("Could not find frame at specified time");
    }

    av_free(buffer);

cleanup:
    if (swsContext) sws_freeContext(swsContext);
    if (packet) av_packet_free(&packet);
    if (frameRGBA) av_frame_free(&frameRGBA);
    if (frame) av_frame_free(&frame);
    if (codecContext) avcodec_free_context(&codecContext);
    if (formatContext) avformat_close_input(&formatContext);
    env->ReleaseStringUTFChars(videoPath, path);

    return result;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoDuration(
        JNIEnv* env,
        jclass clazz,
        jstring videoPath) {

    const char* path = env->GetStringUTFChars(videoPath, nullptr);
    AVFormatContext* formatContext = nullptr;
    jlong duration = -1;

    if (avformat_open_input(&formatContext, path, nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(formatContext, nullptr) >= 0) {
            duration = (formatContext->duration * 1000) / AV_TIME_BASE;
        }
        avformat_close_input(&formatContext);
    }

    env->ReleaseStringUTFChars(videoPath, path);
    return duration;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadata(
        JNIEnv* env,
        jclass clazz,
        jstring videoPath) {

    const char* path = env->GetStringUTFChars(videoPath, nullptr);
    AVFormatContext* formatContext = nullptr;
    jobject result = nullptr;

    if (avformat_open_input(&formatContext, path, nullptr, nullptr) == 0) {
        if (avformat_find_stream_info(formatContext, nullptr) >= 0) {
            // Find video stream
            for (unsigned int i = 0; i < formatContext->nb_streams; i++) {
                if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    AVCodecParameters* codecParams = formatContext->streams[i]->codecpar;

                    // Create HashMap
                    jclass hashMapClass = env->FindClass("java/util/HashMap");
                    jmethodID hashMapInit = env->GetMethodID(hashMapClass, "<init>", "()V");
                    jmethodID hashMapPut = env->GetMethodID(hashMapClass, "put",
                        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");

                    result = env->NewObject(hashMapClass, hashMapInit);

                    // Add metadata
                    jclass integerClass = env->FindClass("java/lang/Integer");
                    jmethodID integerInit = env->GetMethodID(integerClass, "<init>", "(I)V");

                    jclass longClass = env->FindClass("java/lang/Long");
                    jmethodID longInit = env->GetMethodID(longClass, "<init>", "(J)V");

                    // Width
                    env->CallObjectMethod(result, hashMapPut,
                        env->NewStringUTF("width"),
                        env->NewObject(integerClass, integerInit, codecParams->width));

                    // Height
                    env->CallObjectMethod(result, hashMapPut,
                        env->NewStringUTF("height"),
                        env->NewObject(integerClass, integerInit, codecParams->height));

                    // Duration
                    jlong duration = (formatContext->duration * 1000) / AV_TIME_BASE;
                    env->CallObjectMethod(result, hashMapPut,
                        env->NewStringUTF("duration"),
                        env->NewObject(longClass, longInit, duration));

                    break;
                }
            }
        }
        avformat_close_input(&formatContext);
    }

    env->ReleaseStringUTFChars(videoPath, path);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getFFmpegVersion(
        JNIEnv* env,
        jclass clazz) {
    return env->NewStringUTF(av_version_info());
}
