# Структура проекта Smart FFmpeg Android

## Финальная структура папки src/

```
src/
└── main/
    ├── cpp/
    │   ├── CMakeLists.txt              # CMake конфигурация (2 библиотеки)
    │   ├── jni_bridge.c                # JNI для SmartFFmpegPlayer
    │   ├── ffmpeg_bridge.cpp           # JNI для SmartFfmpegBridge
    │   └── native_media_engine/        # Нативный движок FFmpeg
    │       ├── ffmpeg_player/          # 50 файлов плеера (*.c, *.h)
    │       │   ├── ffmpeg_player.c/h   # Основной плеер
    │       │   ├── audio_renderer.c/h  # Аудио рендеринг
    │       │   ├── video_renderer.c/h  # Видео рендеринг
    │       │   ├── avsync_gate.c/h     # Синхронизация A/V
    │       │   └── ... (46 других файлов)
    │       ├── include/                # Заголовки FFmpeg
    │       │   ├── libavcodec/
    │       │   ├── libavformat/
    │       │   ├── libavutil/
    │       │   ├── libswscale/
    │       │   └── libswresample/
    │       └── jniLibs/                # Скомпилированные библиотеки
    │           └── arm64-v8a/
    │               ├── libavcodec.so
    │               ├── libavformat.so
    │               ├── libavutil.so
    │               ├── libswscale.so
    │               ├── libswresample.so
    │               ├── libavfilter.so
    │               └── libavdevice.so
    ├── kotlin/com/smartmedia/ffmpeg/
    │   ├── SmartFFmpegPlayer.kt        # API для воспроизведения видео
    │   └── SmartFfmpegBridge.kt        # API для thumbnail/metadata
    └── AndroidManifest.xml

```

## Две нативные библиотеки

### 1. libsmart_ffmpeg.so

- Исходники: `jni_bridge.c` + все файлы из `native_media_engine/ffmpeg_player/`
- Назначение: Воспроизведение видео
- Kotlin API: `SmartFFmpegPlayer.kt`
- Функции:
  - Воспроизведение видео с синхронизацией A/V
  - Seek с точностью до кадра
  - Управление скоростью
  - Поддержка аппаратного ускорения

### 2. libffmpeg_bridge.so

- Исходники: `ffmpeg_bridge.cpp`
- Назначение: Утилиты для работы с видео
- Kotlin API: `SmartFfmpegBridge.kt`
- Функции:
  - Извлечение thumbnail
  - Получение метаданных (duration, width, height)
  - Получение версии FFmpeg

## Что было исправлено

1. ✅ Перенесли `native_media_engine` из корня в `src/main/cpp/`
2. ✅ Переместили `SmartFfmpegBridge.kt` из `java/` в `kotlin/`
3. ✅ Удалили пустую папку `src/main/java/`
4. ✅ Обновили `CMakeLists.txt` для сборки двух библиотек
5. ✅ Обновили документацию

## Преимущества структуры

- Соответствует стандартам Android NDK
- Разделение ответственности (плеер vs утилиты)
- Легко поддерживать и расширять
- Чистая структура без дублирования
