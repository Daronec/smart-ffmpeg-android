/// 🔴 ЗАДАЧА 4: Lifecycle management для Native FFmpeg Player

#ifndef FFMPEG_PLAYER_LIFECYCLE_H
#define FFMPEG_PLAYER_LIFECYCLE_H

#include "ffmpeg_player.h"

/// Присоединить ANativeWindow к плееру
///
/// Создаёт EGLSurface и запускает render loop
/// @param ctx Контекст плеера
/// @param window ANativeWindow (из Flutter SurfaceTexture)
/// @return 0 при успехе, <0 при ошибке
int player_attach_window(PlayerContext *ctx, void *window);

/// Отсоединить ANativeWindow от плеера
///
/// Останавливает render loop и освобождает EGLSurface
/// @param ctx Контекст плеера
void player_detach_window(PlayerContext *ctx);

/// Запустить render loop
///
/// @param ctx Контекст плеера
/// @return 0 при успехе, <0 при ошибке
int render_loop_start(PlayerContext *ctx);

/// Остановить render loop
///
/// @param ctx Контекст плеера
void render_loop_stop(PlayerContext *ctx);

/// Pause при app background
///
/// @param ctx Контекст плеера
void player_pause_lifecycle(PlayerContext *ctx);

/// Resume при app foreground
///
/// @param ctx Контекст плеера
void player_resume_lifecycle(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.4: Native API для background playback
///
/// Переводит плеер в режим audio-only (background)
/// - Останавливает render loop
/// - Отсоединяет surface (SAFE)
/// - Приостанавливает video decode
/// - Продолжает audio playback
/// - Переключает AVSYNC на audio master
///
/// @param ctx Контекст плеера
void native_on_background(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 12.4: Native API для foreground playback
///
/// Возвращает плеер в режим AV (foreground)
/// - Подключает surface
/// - Перезапускает render loop
/// - Возобновляет video decode
/// - Переключает AVSYNC на audio master (до first frame)
///
/// @param ctx Контекст плеера
void native_on_foreground(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.2: API для frame stepping
///
/// Покадровая навигация (← / →, PTS-accurate, без рассинхрона)
/// - direction: +1 → next frame, -1 → previous frame
/// - Audio полностью отключён
/// - AVSYNC не работает
/// - Render loop в auto-режиме запрещён
/// - Video clock управляется вручную
/// - Один decode → один render → стоп
///
/// @param ctx Контекст плеера
/// @param direction Направление: +1 (next) или -1 (previous)
void native_step_frame(PlayerContext *ctx, int direction);

/// 🔴 ШАГ 5: ЭТАЛОННЫЙ player_shutdown() - останавливает ВСЁ (ffplay-grade)
///
/// Это единственная функция, через которую ВСЕГДА закрывается плеер.
/// После вызова не должен работать НИ ОДИН поток.
///
/// Инварианты после player_shutdown(ctx):
/// - ❌ render loop НЕ крутится
/// - ❌ decode threads НЕ работают
/// - ❌ audio callback НЕ вызывается
/// - ❌ frame / packet queue НЕ ждут
/// - ❌ getPosition НЕ увеличивается
/// - ❌ логов НЕТ
///
/// @param ctx Контекст плеера
void player_shutdown(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-CODE-DIFF - запустить AVSYNC watchdog thread
///
/// 🔥 КРИТИЧЕСКИЙ FIX: Вызывается ТОЛЬКО после play()
/// Watchdog должен стартовать когда clocks начали тикать
/// Иначе для video-only файлов watchdog будет считать idle clock как stall
/// @param ctx Контекст плеера
/// @return 0 при успехе, <0 при ошибке
int avsync_watchdog_start(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: AUTO-NEXT - остановить AVSYNC watchdog thread
///
/// Вызывается при EOF для предотвращения ложных срабатываний
/// EOF ≠ STALL - это нормальное завершение playback
/// @param ctx Контекст плеера
void avsync_watchdog_stop(PlayerContext *ctx);

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - запустить seek watchdog thread
///
/// Вызывается при начале seek для мониторинга deadlock
/// @param ctx Контекст плеера
/// @return 0 при успехе, <0 при ошибке
int seek_watchdog_start(PlayerContext *ctx);

/// Остановить seek watchdog thread
///
/// @param ctx Контекст плеера
void seek_watchdog_stop(PlayerContext *ctx);

#endif // FFMPEG_PLAYER_LIFECYCLE_H

