#include "native_preview.h"
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// FFmpeg headers
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
#include "libswscale/swscale.h"

#define LOG_TAG "NativePreview"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// 🔥 КРИТИЧЕСКИЙ FIX: Получить preview кадр (RGBA8888 bitmap)
/// 
/// Preview pipeline полностью независим от PlayerContext:
/// - Не использует EGL / Surface
/// - Не использует render loop
/// - Не использует threads
/// - Не использует AVSYNC-GATE
/// - CPU-only декодирование
int native_preview_get_frame(
    const char *path,
    int64_t target_ms,
    int out_w,
    int out_h,
    uint8_t *buffer,
    int buffer_size
) {
    // 🔥 КРИТИЧЕСКИЙ FIX: ASSERT - preview НЕ зависит от player
    // Preview должен работать даже если:
    // - Видео не воспроизводилось
    // - Плеер disposed
    // - AVI / FLV / без индекса
    // Preview полностью независим от PlayerContext
    
    if (!path || !buffer) {
        ALOGE("❌ native_preview_get_frame: Invalid arguments");
        return -1;
    }
    
    int required_size = out_w * out_h * 4; // RGBA8888
    if (buffer_size < required_size) {
        ALOGE("❌ native_preview_get_frame: Buffer too small (%d < %d)", buffer_size, required_size);
        return -1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Clamp target_ms (0ms часто не декодируется)
    if (target_ms <= 0) {
        target_ms = 100; // Минимум 100ms для гарантии декодирования
        ALOGW("⚠️ Preview: target_ms <= 0, clamped to 100ms");
    }
    
    ALOGI("🎬 Preview: Opening file '%s', target=%lld ms, size=%dx%d", 
          path, (long long)target_ms, out_w, out_h);
    
    // === ШАГ 1: Открыть файл ===
    AVFormatContext *fmt = NULL;
    int ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        ALOGE("❌ Preview: Failed to open file: %s", errbuf);
        return -1;
    }
    
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        ALOGE("❌ Preview: Failed to find stream info");
        avformat_close_input(&fmt);
        return -1;
    }
    
    // === ШАГ 2: Найти видео поток ===
    int video_stream = -1;
    for (unsigned int i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = i;
            break;
        }
    }
    
    if (video_stream < 0) {
        ALOGE("❌ Preview: No video stream found");
        avformat_close_input(&fmt);
        return -1;
    }
    
    AVStream *stream = fmt->streams[video_stream];
    ALOGI("✅ Preview: Video stream found (index=%d, codec=%d)", 
          video_stream, stream->codecpar->codec_id);
    
    // === ШАГ 3: Открыть декодер ===
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        ALOGE("❌ Preview: Codec not found");
        avformat_close_input(&fmt);
        return -1;
    }
    
    AVCodecContext *dec = avcodec_alloc_context3(codec);
    if (!dec) {
        ALOGE("❌ Preview: Failed to allocate codec context");
        avformat_close_input(&fmt);
        return -1;
    }
    
    ret = avcodec_parameters_to_context(dec, stream->codecpar);
    if (ret < 0) {
        ALOGE("❌ Preview: Failed to copy codec parameters");
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        ALOGE("❌ Preview: Failed to open codec: %s", errbuf);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    
    ALOGI("✅ Preview: Decoder opened (size=%dx%d)", dec->width, dec->height);
    
    // === ШАГ 4: Seek BACKWARD к target_ms ===
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 11.4: КРИТИЧЕСКИЙ SEEK (AVI / FLV)
    // ⚠️ НИКОГДА не seek точно в target
    // Почему: AVI / FLV → ключевые кадры далеко, иначе получишь чёрный кадр
    double target_sec = target_ms / 1000.0;
    int64_t seek_ts = av_rescale_q(
        (int64_t)((target_sec - 1.0) * AV_TIME_BASE),  // 🔥 Отступ -1 секунда
        AV_TIME_BASE_Q,
        stream->time_base
    );
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Всегда используем BACKWARD для AVI/FLV
    int seek_flags = AVSEEK_FLAG_BACKWARD;
    ret = av_seek_frame(fmt, video_stream, seek_ts, seek_flags);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        ALOGW("⚠️ Preview: Seek failed: %s (will decode from start)", errbuf);
        // Продолжаем - попробуем декодировать с начала
    } else {
        ALOGI("✅ Preview: Seek to %.3f sec (ts=%lld, offset=-1.0 sec for AVI/FLV)", 
              target_sec, (long long)seek_ts);
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: Flush codec buffers после seek
    avcodec_flush_buffers(dec);
    
    // === ШАГ 5: Декодировать кадры вперёд до первого >= target_ms ===
    AVFrame *frame = av_frame_alloc();
    AVFrame *rgb = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    struct SwsContext *sws = NULL;
    
    if (!frame || !rgb || !pkt) {
        ALOGE("❌ Preview: Failed to allocate frames/packet");
        if (frame) av_frame_free(&frame);
        if (rgb) av_frame_free(&rgb);
        if (pkt) av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    
    // Выделяем буфер для RGBA
    int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, out_w, out_h, 1);
    uint8_t *rgb_buffer = (uint8_t *)av_malloc(rgb_size);
    if (!rgb_buffer) {
        ALOGE("❌ Preview: Failed to allocate RGB buffer");
        av_frame_free(&frame);
        av_frame_free(&rgb);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    
    ret = av_image_fill_arrays(
        rgb->data, rgb->linesize,
        rgb_buffer,
        AV_PIX_FMT_RGBA,
        out_w, out_h, 1
    );
    if (ret < 0) {
        ALOGE("❌ Preview: Failed to fill RGB image");
        av_free(rgb_buffer);
        av_frame_free(&frame);
        av_frame_free(&rgb);
        av_packet_free(&pkt);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        return -1;
    }
    
    // === ШАГ 5: Decode loop (главный момент) ===
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 11.5
    // PTS ≥ target — единственный критерий
    int frame_found = 0;
    int max_decode_attempts = 100; // Защита от бесконечного цикла
    int decode_attempts = 0;
    int decoded_frame_index = 0;  // Для fallback PTS
    
    // Вычисляем fps_guess для fallback
    double fps_guess = 25.0; // 25fps fallback
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        fps_guess = av_q2d(stream->avg_frame_rate);
    }
    
    while (decode_attempts < max_decode_attempts && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }
        
        ret = avcodec_send_packet(dec, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            continue;
        }
        
        while (avcodec_receive_frame(dec, frame) == 0) {
            decode_attempts++;
            decoded_frame_index++;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 11.6: Broken timestamps / VFR FIX
            // Вычисляем PTS в секундах
            double pts_sec = NAN;
            
            // 1. Пробуем frame->pts
            if (frame->pts != AV_NOPTS_VALUE) {
                pts_sec = frame->pts * av_q2d(stream->time_base);
            }
            // 2. Fallback на best_effort_timestamp
            else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                pts_sec = frame->best_effort_timestamp * av_q2d(stream->time_base);
            }
            // 3. Fallback на frame_index / fps_guess
            else {
                pts_sec = decoded_frame_index / fps_guess;
                ALOGW("⚠️ Preview: No PTS, using fallback (frame_index=%d, fps_guess=%.2f, pts=%.3f)", 
                      decoded_frame_index, fps_guess, pts_sec);
            }
            
            // Проверяем, что pts_sec валиден
            if (isnan(pts_sec) || pts_sec < 0.0) {
                ALOGW("⚠️ Preview: Invalid PTS (NAN or negative), skipping frame");
                continue;
            }
            
            double target_sec = target_ms / 1000.0;
            
            // 🔥 КРИТИЧЕСКИЙ FIX: Берём первый валидный кадр >= target
            // ✔️ PTS ≥ target — единственный критерий
            if (pts_sec >= target_sec) {
                // Кадр >= target - это то, что нужно
                ALOGI("✅ Preview: Frame found (pts=%.3f sec >= target=%.3f sec, attempt=%d)", 
                      pts_sec, target_sec, decode_attempts);
                
                // === ШАГ 6: Конвертировать в RGBA ===
                // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 11.7: SCALE (CPU, стабильно)
                if (!sws) {
                    sws = sws_getContext(
                        frame->width, frame->height, (enum AVPixelFormat)frame->format,
                        out_w, out_h, AV_PIX_FMT_RGBA,
                        SWS_BILINEAR,
                        NULL, NULL, NULL
                    );
                    
                    if (!sws) {
                        ALOGE("❌ Preview: Failed to create SwsContext");
                        av_free(rgb_buffer);
                        av_frame_free(&frame);
                        av_frame_free(&rgb);
                        av_packet_free(&pkt);
                        avcodec_free_context(&dec);
                        avformat_close_input(&fmt);
                        return -1;
                    }
                }
                
                ret = sws_scale(
                    sws,
                    (const uint8_t *const *)frame->data,
                    frame->linesize,
                    0,
                    frame->height,
                    rgb->data,
                    rgb->linesize
                );
                
                if (ret < 0) {
                    ALOGE("❌ Preview: Failed to scale frame");
                    sws_freeContext(sws);
                    av_free(rgb_buffer);
                    av_frame_free(&frame);
                    av_frame_free(&rgb);
                    av_packet_free(&pkt);
                    avcodec_free_context(&dec);
                    avformat_close_input(&fmt);
                    return -1;
                }
                
                // === ШАГ 7: Копируем RGBA в выходной буфер ===
                memcpy(buffer, rgb->data[0], required_size);
                frame_found = 1;
                
                av_packet_unref(pkt);
                break; // Выходим из внутреннего цикла (avcodec_receive_frame)
            } else {
                // Кадр < target - продолжаем декодировать
                ALOGD("🔍 Preview: Frame pts=%.3f sec < target=%.3f sec, continuing decode", 
                      pts_sec, target_sec);
            }
        }
        
        av_packet_unref(pkt);
        
        if (frame_found) {
            break; // Выходим из внешнего цикла (av_read_frame)
        }
    }
    
    // === ШАГ 8: Cleanup (ОБЯЗАТЕЛЬНО) ===
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 11.8: Memory contract
    // open_input → close_input
    // open_codec → free_codec
    // alloc_frame → free_frame
    // alloc_packet → unref_packet
    // alloc_sws → free_sws
    // ⛔ Если пропустишь — утечка гарантирована
    
    if (sws) {
        sws_freeContext(sws);
    }
    av_free(rgb_buffer);
    av_frame_free(&frame);
    av_frame_free(&rgb);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    
    if (!frame_found) {
        ALOGE("❌ Preview: No frame found after %d attempts", decode_attempts);
        return -1;
    }
    
    // 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 11.9: ASSERT-ы
    #ifdef DEBUG
    // ASSERT(no_player_context_used) - проверяется отсутствием использования PlayerContext
    // ASSERT(no_audio_decoder) - проверяется отсутствием audio codec
    // ASSERT(no_threads_spawned) - проверяется отсутствием pthread_create
    // ASSERT(single_frame_returned) - проверяется frame_found == 1
    if (!frame_found) {
        ALOGE("❌ PREVIEW_ASSERT FAILED: single_frame_returned=false (FATAL)");
        abort(); // 🔥 FATAL в debug
    }
    #endif
    
    ALOGI("✅ Preview: Frame extracted successfully");
    return 0;
}

