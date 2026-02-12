/// Шаг 40: HDR / colorspace / BT.709 vs BT.2020
///
/// Структура для хранения информации о цветовом пространстве видео

#ifndef VIDEO_COLOR_INFO_H
#define VIDEO_COLOR_INFO_H

#include "libavutil/pixfmt.h"
#include <stdbool.h>

/// Информация о цветовом пространстве видео (Шаг 40.2)
typedef struct VideoColorInfo {
    /// Colorspace (BT.601, BT.709, BT.2020)
    enum AVColorSpace colorspace;
    
    /// Color primaries (BT.601, BT.709, BT.2020)
    enum AVColorPrimaries primaries;
    
    /// Transfer characteristic (BT.709, SMPTE2084, ARIB_STD_B67)
    enum AVColorTransferCharacteristic trc;
    
    /// Color range (LIMITED, FULL)
    enum AVColorRange range;
    
    /// Флаг HDR (Шаг 40.6)
    bool is_hdr;
} VideoColorInfo;

/// Извлечь VideoColorInfo из AVFrame (Шаг 40.2)
///
/// @param frame AVFrame
/// @param info Структура для заполнения
void video_color_info_from_frame(void *frame, VideoColorInfo *info);

/// Проверить, является ли видео HDR (Шаг 40.6)
///
/// @param info VideoColorInfo
/// @return true если HDR
bool video_color_info_is_hdr(const VideoColorInfo *info);

/// Получить colorspace для shader (Шаг 40.3)
///
/// @param info VideoColorInfo
/// @return Индекс colorspace (0=BT.601, 1=BT.709, 2=BT.2020)
int video_color_info_get_colorspace_index(const VideoColorInfo *info);

/// Получить range для shader (Шаг 40.5)
///
/// @param info VideoColorInfo
/// @return 0=LIMITED, 1=FULL
int video_color_info_get_range_index(const VideoColorInfo *info);

#endif // VIDEO_COLOR_INFO_H

