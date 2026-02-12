/// 🔴 ЗАДАЧА 5: Error handling для Native FFmpeg Player

#ifndef FFMPEG_PLAYER_ERROR_H
#define FFMPEG_PLAYER_ERROR_H

#include "ffmpeg_player.h"

/// Типы ошибок плеера (строго ограниченный список)
typedef enum {
    PLAYER_ERROR_NONE = 0,

    // Init
    PLAYER_ERROR_OPEN_FAILED,
    PLAYER_ERROR_NO_STREAMS,
    PLAYER_ERROR_UNSUPPORTED_CODEC,

    // Decode
    PLAYER_ERROR_DECODE_VIDEO,
    PLAYER_ERROR_DECODE_AUDIO,

    // Render
    PLAYER_ERROR_EGL,
    PLAYER_ERROR_GL,

    // Runtime
    PLAYER_ERROR_RENDER_LOOP_DIED,
    PLAYER_ERROR_AUDIO_UNDERRUN,

    // Fatal
    PLAYER_ERROR_INTERNAL
} PlayerError;

/// Установить ошибку в PlayerContext (atomic + single-shot)
///
/// Первая ошибка - главная, остальные игнорируются
/// @param ctx Контекст плеера
/// @param err Тип ошибки
void player_set_error(PlayerContext *ctx, PlayerError err);

/// Получить ошибку из PlayerContext
///
/// @param ctx Контекст плеера
/// @return Тип ошибки (PLAYER_ERROR_NONE если нет ошибки)
PlayerError player_get_error(PlayerContext *ctx);

/// Обработать фатальную ошибку
///
/// Останавливает render loop и audio, но НЕ освобождает ресурсы
/// @param ctx Контекст плеера
/// @param err Тип ошибки
void player_handle_fatal_error(PlayerContext *ctx, PlayerError err);

#endif // FFMPEG_PLAYER_ERROR_H

