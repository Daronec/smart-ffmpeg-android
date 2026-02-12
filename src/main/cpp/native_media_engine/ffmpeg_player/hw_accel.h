#ifndef HW_ACCEL_H
#define HW_ACCEL_H

#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include <stdbool.h>

/// Тип аппаратного ускорения
typedef enum {
    HW_ACCEL_NONE,      // Software decode
    HW_ACCEL_MEDIACODEC, // Android MediaCodec
    HW_ACCEL_VIDEOTOOLBOX, // iOS VideoToolbox
} HWAccelType;

/// Состояние аппаратного ускорения
typedef struct {
    /// Тип ускорения
    HWAccelType type;
    
    /// HW device context (для FFmpeg)
    AVBufferRef *hw_device_ctx;
    
    /// HW frames context (для декодера)
    AVBufferRef *hw_frames_ctx;
    
    /// Флаг, что HW инициализирован
    bool initialized;
    
    /// Флаг, что HW разрешён для этого формата/кодека
    bool allowed;
    
    /// Флаг, что HW уже упал (fallback выполнен)
    bool failed;
} HWAccelState;

/// Чёрный список форматов для HW (всегда software)
static const char *const HW_BLACKLIST_FORMATS[] = {
    "avi", "flv", "divx", "xvid", "mpg", "mpeg", "wmv", NULL
};

/// Чёрный список кодеков для HW (всегда software)
static const char *const HW_BLACKLIST_CODECS[] = {
    "mpeg4", "mp4v", "xvid", "divx", NULL
};

/// Чёрный список устройств для HW (всегда software)
static const char *const HW_BLACKLIST_DEVICES[] = {
    "hisi", "hisilicon", "kirin", "mtk", "mediatek", "rockchip", "allwinner", NULL
};

/// Инициализировать состояние HW acceleration
///
/// @param hw Состояние для инициализации
void hw_accel_init(HWAccelState *hw);

/// Освободить ресурсы HW acceleration
///
/// @param hw Состояние
void hw_accel_destroy(HWAccelState *hw);

/// Проверить, разрешено ли HW для формата/кодека/устройства
///
/// @param format Формат контейнера (mp4, mkv, etc.)
/// @param codec_id ID кодека (AV_CODEC_ID_H264, etc.)
/// @param device_name Имя устройства (для blacklist)
/// @return true если HW разрешён
bool hw_accel_is_allowed(const char *format, enum AVCodecID codec_id, const char *device_name);

/// Попытаться инициализировать HW device
///
/// @param hw Состояние
/// @param codec_id ID кодека
/// @return 0 при успехе, <0 при ошибке
int hw_accel_init_device(HWAccelState *hw, enum AVCodecID codec_id);

/// Попытаться открыть кодек с HW acceleration
///
/// @param hw Состояние
/// @param codec_ctx Codec context
/// @param codec Кодек
/// @return 0 при успехе, <0 при ошибке (fallback на software)
int hw_accel_open_codec(HWAccelState *hw, AVCodecContext *codec_ctx, const AVCodec *codec);

/// Отключить HW acceleration (fallback на software)
///
/// @param hw Состояние
void hw_accel_disable(HWAccelState *hw);

/// Получить HW pixel format (если используется HW)
///
/// @param hw Состояние
/// @return HW pixel format, или AV_PIX_FMT_NONE если не используется
enum AVPixelFormat hw_accel_get_pixel_format(HWAccelState *hw);

/// Проверить, используется ли HW acceleration
///
/// @param hw Состояние
/// @return true если используется
bool hw_accel_is_active(HWAccelState *hw);

/// Проверить, является ли устройство HiSilicon/Kirin
///
/// @return true если устройство HiSilicon/Kirin
bool is_hisilicon_device(void);

#endif // HW_ACCEL_H

