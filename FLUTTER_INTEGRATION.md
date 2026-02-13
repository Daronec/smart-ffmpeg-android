# Интеграция smart-ffmpeg-android v1.0.4 в Flutter плагин

## Быстрый старт

### 1. Обновите зависимость

В файле `android/build.gradle` или `android/build.gradle.kts` вашего Flutter плагина:

```kotlin
dependencies {
    implementation("org.jetbrains.kotlin:kotlin-stdlib:1.9.0")
    implementation("com.smartmedia:smart-ffmpeg-android:1.0.4")  // ← Обновите версию
}
```

### 2. Используйте SmartFfmpegBridge

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge

class YourPlugin : FlutterPlugin, MethodCallHandler {

    override fun onMethodCall(call: MethodCall, result: Result) {
        when (call.method) {
            "getFFmpegVersion" -> {
                try {
                    val version = SmartFfmpegBridge.getFFmpegVersion()
                    result.success(version)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }

            "extractThumbnail" -> {
                val videoPath = call.argument<String>("videoPath")
                val timeMs = call.argument<Long>("timeMs") ?: 0L
                val width = call.argument<Int>("width") ?: 0
                val height = call.argument<Int>("height") ?: 0

                try {
                    val thumbnail = SmartFfmpegBridge.extractThumbnail(
                        videoPath = videoPath!!,
                        timeMs = timeMs,
                        width = width,
                        height = height
                    )

                    if (thumbnail != null) {
                        result.success(thumbnail)
                    } else {
                        result.error("ERROR", "Failed to extract thumbnail", null)
                    }
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }

            "getVideoDuration" -> {
                val videoPath = call.argument<String>("videoPath")

                try {
                    val duration = SmartFfmpegBridge.getVideoDuration(videoPath!!)
                    result.success(duration)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }

            "getVideoMetadata" -> {
                val videoPath = call.argument<String>("videoPath")

                try {
                    val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath!!)
                    result.success(metadata)
                } catch (e: Exception) {
                    result.error("ERROR", e.message, null)
                }
            }

            else -> result.notImplemented()
        }
    }
}
```

### 3. Очистите кеш и пересоберите

```bash
cd your_flutter_plugin
flutter clean
cd example
flutter clean
flutter pub get
flutter build apk
```

## Важные замечания

### ✅ Что исправлено в v1.0.4

1. **JNI методы реализованы**: Все 4 метода (`extractThumbnail`, `getVideoDuration`, `getVideoMetadata`, `getFFmpegVersion`) полностью реализованы в C коде
2. **Правильные JNI имена**: Убран `_00024Companion` из имен методов
3. **Kotlin object**: `SmartFfmpegBridge` теперь singleton (не нужен `companion object`)
4. **RGBA формат**: Миниатюры возвращаются в формате RGBA (4 байта на пиксель)

### 📋 Требования

- **Minimum SDK**: 26 (Android 8.0)
- **Target SDK**: 34
- **Архитектура**: arm64-v8a
- **Путь к видео**: Должен быть абсолютным путем к файлу

### 🔍 Отладка

Если возникают проблемы, проверьте логи:

```bash
adb logcat | grep SmartFfmpegBridge
```

Вы должны увидеть сообщения типа:

```
I/SmartFfmpegBridge: Extracting thumbnail from: /path/to/video.mp4 at 5000 ms, size: 640x360
I/SmartFfmpegBridge: Successfully extracted thumbnail: 921600 bytes
```

### 📦 Формат данных

#### extractThumbnail

- **Возвращает**: `ByteArray?` (RGBA формат)
- **Размер**: `width * height * 4` байт
- **Формат пикселя**: R, G, B, A (по 1 байту каждый)

#### getVideoDuration

- **Возвращает**: `Long` (миллисекунды)
- **Ошибка**: `-1` если не удалось получить длительность

#### getVideoMetadata

- **Возвращает**: `Map<String, Any>?`
- **Ключи**:
  - `width`: Int - ширина видео
  - `height`: Int - высота видео
  - `duration`: Long - длительность в мс
  - `codec`: String - название кодека
  - `bitrate`: Long - битрейт

#### getFFmpegVersion

- **Возвращает**: `String` - версия FFmpeg (например, "n4.4.2")

## Пример Flutter кода

```dart
import 'package:flutter/services.dart';

class VideoThumbnailPlugin {
  static const MethodChannel _channel = MethodChannel('video_thumbnail');

  static Future<String?> getFFmpegVersion() async {
    try {
      final String version = await _channel.invokeMethod('getFFmpegVersion');
      return version;
    } catch (e) {
      print('Error getting FFmpeg version: $e');
      return null;
    }
  }

  static Future<Uint8List?> extractThumbnail({
    required String videoPath,
    required int timeMs,
    int width = 0,
    int height = 0,
  }) async {
    try {
      final Uint8List? thumbnail = await _channel.invokeMethod(
        'extractThumbnail',
        {
          'videoPath': videoPath,
          'timeMs': timeMs,
          'width': width,
          'height': height,
        },
      );
      return thumbnail;
    } catch (e) {
      print('Error extracting thumbnail: $e');
      return null;
    }
  }

  static Future<int?> getVideoDuration(String videoPath) async {
    try {
      final int duration = await _channel.invokeMethod(
        'getVideoDuration',
        {'videoPath': videoPath},
      );
      return duration;
    } catch (e) {
      print('Error getting video duration: $e');
      return null;
    }
  }

  static Future<Map<String, dynamic>?> getVideoMetadata(String videoPath) async {
    try {
      final Map<dynamic, dynamic> metadata = await _channel.invokeMethod(
        'getVideoMetadata',
        {'videoPath': videoPath},
      );
      return Map<String, dynamic>.from(metadata);
    } catch (e) {
      print('Error getting video metadata: $e');
      return null;
    }
  }
}
```

## Тестирование

```dart
void testVideoThumbnail() async {
  // 1. Проверить версию FFmpeg
  final version = await VideoThumbnailPlugin.getFFmpegVersion();
  print('FFmpeg version: $version');

  // 2. Получить длительность
  final duration = await VideoThumbnailPlugin.getVideoDuration('/path/to/video.mp4');
  print('Duration: ${duration}ms');

  // 3. Получить метаданные
  final metadata = await VideoThumbnailPlugin.getVideoMetadata('/path/to/video.mp4');
  print('Metadata: $metadata');

  // 4. Извлечь миниатюру
  final thumbnail = await VideoThumbnailPlugin.extractThumbnail(
    videoPath: '/path/to/video.mp4',
    timeMs: 5000,
    width: 640,
    height: 360,
  );

  if (thumbnail != null) {
    print('Thumbnail extracted: ${thumbnail.length} bytes');
    // Конвертировать в Image
    // final image = Image.memory(thumbnail);
  }
}
```

## Поддержка

- **GitHub**: https://github.com/Daronec/smart-ffmpeg-android
- **Issues**: https://github.com/Daronec/smart-ffmpeg-android/issues
- **Версия**: 1.0.4
