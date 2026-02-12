/// 🔴 ЗАДАЧА 6: Subtitles API для Flutter
///
/// Минимальный API для управления субтитрами из Dart.
/// Вся логика парсинга, тайминга и рендеринга остаётся в native коде.

#include "subtitle_api.h"
#include "subtitle_manager.h"
#include "ffmpeg_player_error.h"
#include <string.h>
#include <strings.h>  // Для strcasecmp
#include <android/log.h>

#define LOG_TAG "SubtitleAPI"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

/// Определить формат субтитров по расширению файла
static int detect_subtitle_format(const char *path) {
    if (!path) {
        return -1;
    }
    
    const char *ext = strrchr(path, '.');
    if (!ext) {
        return -1;
    }
    
    ext++; // Пропускаем точку
    
    if (strcasecmp(ext, "srt") == 0) {
        return 0; // SRT
    } else if (strcasecmp(ext, "ass") == 0 || strcasecmp(ext, "ssa") == 0) {
        return 1; // ASS/SSA
    }
    
    return -1;
}

/// Загрузить внешние субтитры (.srt / .ass)
int subtitle_load(PlayerContext *ctx, const char *path) {
    if (!ctx || !path) {
        ALOGE("subtitle_load: Invalid parameters");
        return -1;
    }
    
    ALOGI("🔄 subtitle_load: path=%s", path);
    
    // Очищаем старые субтитры
    subtitle_manager_clear(&ctx->subtitles);
    
    // Определяем формат
    int format = detect_subtitle_format(path);
    if (format < 0) {
        ALOGE("subtitle_load: Unsupported format (expected .srt or .ass/.ssa)");
        player_set_error(ctx, PLAYER_ERROR_INTERNAL);
        return -1;
    }
    
    // Загружаем в зависимости от формата
    int ret;
    if (format == 0) {
        // SRT
        ret = subtitle_manager_parse_srt(&ctx->subtitles, path);
    } else {
        // ASS/SSA
        ret = subtitle_manager_parse_ass(&ctx->subtitles, path);
    }
    
    if (ret < 0) {
        ALOGE("subtitle_load: Failed to parse subtitle file: %s", path);
        player_set_error(ctx, PLAYER_ERROR_INTERNAL);
        return -1;
    }
    
    // Включаем субтитры после загрузки
    ctx->subtitles_enabled = 1;
    
    ALOGI("✅ subtitle_load: Loaded %d subtitles", ctx->subtitles.count);
    return 0;
}

/// Включить / выключить субтитры
void subtitle_enable(PlayerContext *ctx, int enable) {
    if (!ctx) {
        return;
    }
    
    ctx->subtitles_enabled = enable ? 1 : 0;
    ALOGI("✅ subtitle_enable: %s", enable ? "enabled" : "disabled");
}

/// Очистить все субтитры
void subtitle_clear(PlayerContext *ctx) {
    if (!ctx) {
        return;
    }
    
    subtitle_manager_clear(&ctx->subtitles);
    ctx->subtitles_enabled = 0;
    ALOGI("✅ subtitle_clear: Subtitles cleared");
}

