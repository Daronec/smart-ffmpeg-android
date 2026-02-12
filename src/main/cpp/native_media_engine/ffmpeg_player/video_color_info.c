/// Шаг 40: HDR / colorspace / BT.709 vs BT.2020

#include "video_color_info.h"
#include "libavutil/frame.h"
#include <string.h>

void video_color_info_from_frame(void *frame, VideoColorInfo *info) {
    if (!frame || !info) {
        return;
    }
    
    AVFrame *f = (AVFrame *)frame;
    
    // Шаг 40.2: Извлекаем информацию из AVFrame
    info->colorspace = f->colorspace;
    info->primaries = f->color_primaries;
    info->trc = f->color_trc;
    info->range = f->color_range;
    
    // Шаг 40.6: HDR detection
    info->is_hdr = video_color_info_is_hdr(info);
}

bool video_color_info_is_hdr(const VideoColorInfo *info) {
    if (!info) {
        return false;
    }
    
    // Шаг 40.6: HDR detection
    // HDR = BT.2020 primaries + (PQ или HLG transfer)
    bool is_hdr = (info->primaries == AVCOL_PRI_BT2020) &&
                  (info->trc == AVCOL_TRC_SMPTE2084 ||  // HDR10 (PQ)
                   info->trc == AVCOL_TRC_ARIB_STD_B67); // HLG
    
    return is_hdr;
}

int video_color_info_get_colorspace_index(const VideoColorInfo *info) {
    if (!info) {
        return 1; // BT.709 по умолчанию
    }
    
    // Шаг 40.3: Определяем индекс colorspace для shader
    switch (info->colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return 0; // BT.601 (BT601 удалён из FFmpeg, используем BT470BG/SMPTE170M)
        
        case AVCOL_SPC_BT709:
            return 1; // BT.709
        
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return 2; // BT.2020
        
        default:
            // Fallback на BT.709 для неизвестных
            return 1;
    }
}

int video_color_info_get_range_index(const VideoColorInfo *info) {
    if (!info) {
        return 0; // LIMITED по умолчанию
    }
    
    // Шаг 40.5: Color range
    return (info->range == AVCOL_RANGE_JPEG) ? 1 : 0; // 0=LIMITED, 1=FULL
}

