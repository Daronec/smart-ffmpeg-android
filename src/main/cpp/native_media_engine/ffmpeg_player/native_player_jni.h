#ifndef NATIVE_PLAYER_JNI_H
#define NATIVE_PLAYER_JNI_H

#include "video_render_gl.h"
#include "ffmpeg_player.h"  // Для полного определения PlayerContext

/// 🔴 ЭТАЛОН: Уведомить Flutter о новом кадре для ImageTexture
/// Вызывается из video_render_gl_mark_frame_available после успешного рендеринга
/// Использует FlutterJNI.markTextureFrameAvailable(textureId) напрямую
/// @param texture_id Flutter texture ID
void native_player_mark_frame_available(int64_t texture_id);

/// Устанавливает VideoRenderGL instance (вызывается из video_render_gl_init)
void native_player_set_renderer(VideoRenderGL *renderer);

/// Очищает все глобальные ссылки (вызывается при завершении)
void native_player_cleanup(void);

/// Проверить, что PlayerContext не в состоянии abort (для защиты от вызовов после dispose)
/// @return 1 если abort установлен (нельзя вызывать callbacks), 0 если можно
int native_player_is_aborted(void);

/// 🔴 ШАГ 4: Отправить событие prepared в Flutter
///
/// Вызывается из video_decode_thread когда первый кадр успешно добавлен в очередь.
/// Отправляет событие через MethodChannel в Kotlin, который затем отправляет в Dart.
/// @param has_audio 1 если есть аудио, 0 если video-only
void native_player_emit_prepared_event(int has_audio);

/// 🔴 ЭТАЛОН: Отправить prepared event с has_audio и duration
/// @param has_audio 1 если есть аудио, 0 если video-only
/// @param duration_ms Длительность в миллисекундах
void native_player_emit_prepared_event_with_data(PlayerContext *ctx, int has_audio, int64_t duration_ms);

/// 🔴 ЭТАЛОН: Отправить duration в Flutter
///
/// Вызывается после prepare, когда duration вычислен.
/// Отправляет duration через MethodChannel в Kotlin.
/// @param duration_ms Длительность в миллисекундах
void native_player_emit_duration_event(int64_t duration_ms);

/// 🔥 КРИТИЧЕСКИЙ FIX: Отправить surface_ready event в Flutter
///
/// Вызывается из render loop ПОСЛЕ успешного eglMakeCurrent().
/// Это критично для TEXTURE-RACE fix - render loop должен стартовать ТОЛЬКО после eglMakeCurrent.
/// surfaceReady = EGLSurface создан и eglMakeCurrent успешно выполнен.
void native_player_emit_surface_ready_event(void);

/// 🔒 FIX Z25: Отправить first_frame event в Flutter
///
/// Вызывается из render loop ПОСЛЕ eglSwapBuffers(), когда первый кадр реально отрисован.
/// Это критично для скрытия loader в UI - loader скрывается ТОЛЬКО после реального рендера первого кадра.
/// prepared ≠ first frame - prepared означает metadata OK, first_frame означает кадр на экране.
void native_player_emit_first_frame_event(void);

/// 🔥 КРИТИЧЕСКИЙ FIX: Отправить firstFrameAfterSeek event в Flutter
///
/// Вызывается из render loop ПОСЛЕ eglSwapBuffers(), когда первый кадр после seek реально отрисован.
/// Это критично для AVI/FLV - seek должен ждать реального кадра >= target перед переходом в ready/playing.
/// firstFrameAfterSeek = гарантия, что кадр на экране соответствует seek_target.
void native_player_emit_first_frame_after_seek_event(void);

/// 🔥 КРИТИЧЕСКИЙ FIX: AudioState Contract (RFC v1) - эмит события изменения AudioState
/// Эмитится ТОЛЬКО из native-кода при переходах состояний
/// @param state Строковое представление AudioState: "noAudio", "initializing", "initialized", "playing", "paused", "stoppedBySystem", "dead"
void native_player_emit_audio_state_event(const char *state);

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC-MASTER - эмит события ошибки (FATAL условия)
/// Эмитится при обнаружении FATAL условий: AUDIO_MASTER_LOST, CLOCK_STALL, DRIFT_RUNAWAY
/// @param message Сообщение об ошибке
void native_player_emit_error_event(const char *message);

/// 🔥 КРИТИЧЕСКИЙ FIX: SEEK + AVSYNC PATCH - ШАГ 13.8: Эмит события frameStepped
/// Эмитится после успешного frame step (next/previous)
/// @param pts_ms PTS кадра в миллисекундах
void native_player_emit_frame_stepped_event(int64_t pts_ms);

/// 🔥 КРИТИЧЕСКИЙ FIX: AVSYNC CODE DIFF - ШАГ 20.5: Эмит диагностического события
/// Эмитится для Flutter HUD с информацией о AVSYNC состоянии
/// @param type Тип события (например, "avsync")
/// @param key Ключ (например, "master", "audio_stalled")
/// @param value Значение (например, "audio", "1")
void native_player_emit_diagnostic_event(const char *type, const char *key, const char *value);

#endif // NATIVE_PLAYER_JNI_H

