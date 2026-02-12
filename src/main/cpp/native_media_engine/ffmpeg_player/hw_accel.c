#include "hw_accel.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <android/log.h>
#include <sys/system_properties.h>
#include "libavutil/hwcontext.h"

#define LOG_TAG "HWAccel"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

void hw_accel_init(HWAccelState *hw) {
    if (!hw) {
        return;
    }
    
    memset(hw, 0, sizeof(HWAccelState));
    hw->type = HW_ACCEL_NONE;
    hw->initialized = false;
    hw->allowed = false;
    hw->failed = false;
}

void hw_accel_destroy(HWAccelState *hw) {
    if (!hw) {
        return;
    }
    
    if (hw->hw_frames_ctx) {
        av_buffer_unref(&hw->hw_frames_ctx);
    }
    
    if (hw->hw_device_ctx) {
        av_buffer_unref(&hw->hw_device_ctx);
    }
    
    hw_accel_init(hw);
}

// Вспомогательная функция для case-insensitive сравнения
static int str_icmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) {
            return c1 - c2;
        }
        s1++;
        s2++;
    }
    return tolower(*s1) - tolower(*s2);
}

/// Проверить, является ли устройство HiSilicon/Kirin
bool is_hisilicon_device(void) {
    #ifdef __ANDROID__
        char prop_value[PROP_VALUE_MAX];
        __system_property_get("ro.board.platform", prop_value);
        
        // Проверяем наличие "hisi" или "kirin" в имени платформы
        char *lower = prop_value;
        for (int i = 0; prop_value[i]; i++) {
            lower[i] = tolower(prop_value[i]);
        }
        
        if (strstr(lower, "hisi") != NULL || strstr(lower, "kirin") != NULL) {
            return true;
        }
        
        // Также проверяем ro.product.board
        __system_property_get("ro.product.board", prop_value);
        for (int i = 0; prop_value[i]; i++) {
            lower[i] = tolower(prop_value[i]);
        }
        
        if (strstr(lower, "hisi") != NULL || strstr(lower, "kirin") != NULL) {
            return true;
        }
    #endif
    return false;
}

bool hw_accel_is_allowed(const char *format, enum AVCodecID codec_id, const char *device_name) {
    // 1. Проверка устройства (Шаг 24.1 - ОБЯЗАТЕЛЬНО первым!)
    #ifdef __ANDROID__
        if (is_hisilicon_device()) {
            ALOGD("HiSilicon/Kirin device detected - HW decode disabled");
            return false;
        }
    #endif
    
    // 2. Проверка формата (чёрный список)
    if (format) {
        for (int i = 0; HW_BLACKLIST_FORMATS[i] != NULL; i++) {
            if (str_icmp(format, HW_BLACKLIST_FORMATS[i]) == 0) {
                ALOGD("Format %s in blacklist - HW decode disabled", format);
                return false;
            }
        }
    }
    
    // 3. Проверка кодека (чёрный список)
    const char *codec_name = avcodec_get_name(codec_id);
    if (codec_name) {
        for (int i = 0; HW_BLACKLIST_CODECS[i] != NULL; i++) {
            if (str_icmp(codec_name, HW_BLACKLIST_CODECS[i]) == 0) {
                ALOGD("Codec %s in blacklist - HW decode disabled", codec_name);
                return false;
            }
        }
        
        // MPEG-4 Part 2 (mp4v-es) - всегда software (Шаг 24)
        if (codec_id == AV_CODEC_ID_MPEG4) {
            ALOGD("MPEG-4 Part 2 detected - HW decode disabled");
            return false;
        }
    }
    
    // 4. Проверка устройства (чёрный список через параметр)
    if (device_name) {
        char device_lower[256];
        strncpy(device_lower, device_name, sizeof(device_lower) - 1);
        device_lower[sizeof(device_lower) - 1] = '\0';
        
        // Приводим к нижнему регистру
        for (int i = 0; device_lower[i]; i++) {
            device_lower[i] = tolower(device_lower[i]);
        }
        
        for (int i = 0; HW_BLACKLIST_DEVICES[i] != NULL; i++) {
            if (strstr(device_lower, HW_BLACKLIST_DEVICES[i]) != NULL) {
                ALOGD("Device %s in blacklist - HW decode disabled", device_name);
                return false;
            }
        }
    }
    
    // 5. Проверка кодека (разрешены только H.264 и H.265 для MediaCodec, Шаг 24)
    switch (codec_id) {
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
            // Разрешены только для MP4/MKV (проверка формата выше)
            return true;
            
        default:
            ALOGD("Codec %s not supported for HW decode", codec_name ? codec_name : "unknown");
            return false;
    }
}

int hw_accel_init_device(HWAccelState *hw, enum AVCodecID codec_id) {
    if (!hw) {
        return -1;
    }
    
    // Определяем тип HW acceleration
    #ifdef __ANDROID__
        hw->type = HW_ACCEL_MEDIACODEC;
    #elif defined(__APPLE__)
        hw->type = HW_ACCEL_VIDEOTOOLBOX;
    #else
        hw->type = HW_ACCEL_NONE;
        return -1;
    #endif
    
    // Инициализируем HW device context
    const char *device_type = NULL;
    
    #ifdef __ANDROID__
        device_type = "mediacodec";
    #elif defined(__APPLE__)
        device_type = "videotoolbox";
    #endif
    
    if (!device_type) {
        return -1;
    }
    
    int ret = av_hwdevice_ctx_create(&hw->hw_device_ctx, 
                                     av_hwdevice_find_type_by_name(device_type),
                                     NULL, NULL, 0);
    
    if (ret < 0) {
        hw->failed = true;
        return ret;
    }
    
    hw->initialized = true;
    return 0;
}

int hw_accel_open_codec(HWAccelState *hw, AVCodecContext *codec_ctx, const AVCodec *codec) {
    if (!hw || !codec_ctx || !codec) {
        return -1;
    }
    
    // Если HW не разрешён или уже упал - используем software
    if (!hw->allowed || hw->failed) {
        return -1;
    }
    
    // Если HW device не инициализирован - пытаемся инициализировать
    if (!hw->initialized) {
        int ret = hw_accel_init_device(hw, codec_ctx->codec_id);
        if (ret < 0) {
            hw->failed = true;
            return ret;
        }
    }
    
    // Устанавливаем HW device context
    codec_ctx->hw_device_ctx = av_buffer_ref(hw->hw_device_ctx);
    
    // Пытаемся открыть кодек
    int ret = avcodec_open2(codec_ctx, codec, NULL);
    
    if (ret < 0) {
        // HW не удалось - отключаем и возвращаем ошибку
        av_buffer_unref(&codec_ctx->hw_device_ctx);
        hw_accel_disable(hw);
        return ret;
    }
    
    return 0;
}

void hw_accel_disable(HWAccelState *hw) {
    if (!hw) {
        return;
    }
    
    hw->failed = true;
    hw->allowed = false;
    
    if (hw->hw_frames_ctx) {
        av_buffer_unref(&hw->hw_frames_ctx);
    }
    
    if (hw->hw_device_ctx) {
        av_buffer_unref(&hw->hw_device_ctx);
    }
    
    hw->initialized = false;
}

enum AVPixelFormat hw_accel_get_pixel_format(HWAccelState *hw) {
    if (!hw || !hw->initialized || hw->failed) {
        return AV_PIX_FMT_NONE;
    }
    
    #ifdef __ANDROID__
        return AV_PIX_FMT_MEDIACODEC;
    #elif defined(__APPLE__)
        return AV_PIX_FMT_VIDEOTOOLBOX;
    #else
        return AV_PIX_FMT_NONE;
    #endif
}

bool hw_accel_is_active(HWAccelState *hw) {
    if (!hw) {
        return false;
    }
    
    return hw->initialized && !hw->failed && hw->allowed;
}

