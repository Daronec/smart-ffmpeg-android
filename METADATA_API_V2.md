# Metadata API v2 - Extended Fields & JSON Support

## 🎉 Что нового в версии 1.0.5

### 1️⃣ Расширенные поля метаданных

Добавлены новые поля в `getVideoMetadata()`:

| Поле           | Тип     | Описание                                    |
| -------------- | ------- | ------------------------------------------- |
| `fps`          | Double  | Частота кадров (frames per second)          |
| `audioCodec`   | String  | Аудио кодек (aac, mp3, opus, etc.)          |
| `streamCount`  | Int     | Общее количество потоков                    |
| `hasAudio`     | Boolean | Наличие аудио дорожки                       |
| `hasSubtitles` | Boolean | Наличие субтитров                           |
| `container`    | String  | Формат контейнера (mp4, avi, mkv, etc.)     |
| `rotation`     | Int     | Поворот видео (0, 90, 180, 270)             |
| `sampleRate`   | Int     | Частота дискретизации аудио (Hz)            |
| `channels`     | Int     | Количество аудио каналов (1=mono, 2=stereo) |

### 2️⃣ JSON метод

Новый метод `getVideoMetadataJson()` возвращает метаданные в JSON формате.

### 3️⃣ Safe-mode

Все методы теперь безопасны - не крашат приложение при ошибках.

---

## 📖 API Reference

### getVideoMetadata() - HashMap

**Kotlin:**

```kotlin
val metadata: Map<String, Any>? = SmartFfmpegBridge.getVideoMetadata(videoPath)

// Базовые поля
val width = metadata?.get("width") as? Int
val height = metadata?.get("height") as? Int
val duration = metadata?.get("duration") as? Long
val codec = metadata?.get("codec") as? String
val bitrate = metadata?.get("bitrate") as? Long

// Новые поля
val fps = metadata?.get("fps") as? Double
val rotation = metadata?.get("rotation") as? Int
val container = metadata?.get("container") as? String
val streamCount = metadata?.get("streamCount") as? Int
val hasAudio = metadata?.get("hasAudio") as? Boolean
val hasSubtitles = metadata?.get("hasSubtitles") as? Boolean

// Аудио поля (если hasAudio == true)
val audioCodec = metadata?.get("audioCodec") as? String
val sampleRate = metadata?.get("sampleRate") as? Int
val channels = metadata?.get("channels") as? Int
```

**Пример вывода:**

```kotlin
{
  "width": 1920,
  "height": 1080,
  "duration": 120000,
  "codec": "h264",
  "bitrate": 5000000,
  "fps": 30.0,
  "rotation": 0,
  "container": "mp4",
  "streamCount": 2,
  "hasAudio": true,
  "hasSubtitles": false,
  "audioCodec": "aac",
  "sampleRate": 48000,
  "channels": 2
}
```

---

### getVideoMetadataJson() - JSON String

**Kotlin:**

```kotlin
val json: String = SmartFfmpegBridge.getVideoMetadataJson(videoPath)
val jsonObject = JSONObject(json)

if (jsonObject.getBoolean("success")) {
    val data = jsonObject.getJSONObject("data")
    val width = data.getInt("width")
    val fps = data.getDouble("fps")
    val hasAudio = data.getBoolean("hasAudio")
} else {
    val error = jsonObject.getString("error")
    Log.e("Video", "Error: $error")
}
```

**Success Response:**

```json
{
  "success": true,
  "data": {
    "width": 1920,
    "height": 1080,
    "duration": 120000,
    "codec": "h264",
    "bitrate": 5000000,
    "fps": 30.0,
    "rotation": 0,
    "container": "mp4",
    "streamCount": 2,
    "hasAudio": true,
    "hasSubtitles": false,
    "audioCodec": "aac",
    "sampleRate": 48000,
    "channels": 2
  }
}
```

**Error Response:**

```json
{
  "success": false,
  "error": "Could not open file: No such file or directory"
}
```

---

## 🦋 Flutter Integration

### Platform Channel

**Kotlin (Android):**

```kotlin
when (call.method) {
    "getVideoMetadataJson" -> {
        val videoPath = call.argument<String>("videoPath")!!
        val json = SmartFfmpegBridge.getVideoMetadataJson(videoPath)
        result.success(json)
    }
}
```

**Dart:**

```dart
Future<Map<String, dynamic>?> getVideoMetadata(String videoPath) async {
  try {
    final String jsonString = await _channel.invokeMethod(
      'getVideoMetadataJson',
      {'videoPath': videoPath},
    );

    final Map<String, dynamic> json = jsonDecode(jsonString);

    if (json['success'] == true) {
      return json['data'] as Map<String, dynamic>;
    } else {
      print('Error: ${json['error']}');
      return null;
    }
  } catch (e) {
    print('Exception: $e');
    return null;
  }
}
```

**Usage:**

```dart
final metadata = await getVideoMetadata('/path/to/video.mp4');

if (metadata != null) {
  print('Resolution: ${metadata['width']}x${metadata['height']}');
  print('FPS: ${metadata['fps']}');
  print('Duration: ${metadata['duration']} ms');
  print('Has audio: ${metadata['hasAudio']}');
  print('Audio codec: ${metadata['audioCodec']}');
  print('Sample rate: ${metadata['sampleRate']} Hz');
  print('Channels: ${metadata['channels']}');
}
```

---

## 🎯 Use Cases

### 1. Video Player Info

```kotlin
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)

// Display video info
val resolution = "${metadata["width"]}x${metadata["height"]}"
val fps = metadata["fps"] as Double
val duration = formatDuration(metadata["duration"] as Long)

textView.text = """
    Resolution: $resolution
    FPS: ${fps.toInt()}
    Duration: $duration
    Codec: ${metadata["codec"]}
""".trimIndent()
```

### 2. Audio Detection

```kotlin
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)
val hasAudio = metadata["hasAudio"] as Boolean

if (!hasAudio) {
    Toast.makeText(context, "This video has no audio", Toast.LENGTH_SHORT).show()
}
```

### 3. Rotation Handling

```kotlin
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)
val rotation = metadata["rotation"] as Int

// Apply rotation to video view
videoView.rotation = rotation.toFloat()
```

### 4. Format Validation

```kotlin
val json = SmartFfmpegBridge.getVideoMetadataJson(videoPath)
val jsonObject = JSONObject(json)

if (!jsonObject.getBoolean("success")) {
    val error = jsonObject.getString("error")

    when {
        error.contains("Could not open") -> {
            // File not found or corrupted
            showError("Invalid video file")
        }
        error.contains("No video stream") -> {
            // Audio-only file
            showError("This file contains no video")
        }
        else -> {
            showError("Unknown error: $error")
        }
    }
}
```

### 5. Audio Quality Check

```kotlin
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)

if (metadata["hasAudio"] == true) {
    val sampleRate = metadata["sampleRate"] as Int
    val channels = metadata["channels"] as Int
    val audioCodec = metadata["audioCodec"] as String

    val quality = when {
        sampleRate >= 48000 && channels >= 2 -> "High Quality"
        sampleRate >= 44100 -> "Good Quality"
        else -> "Low Quality"
    }

    println("Audio: $audioCodec, $sampleRate Hz, $channels ch - $quality")
}
```

---

## 🔒 Error Handling

### Safe-mode гарантии:

1. **Никогда не крашит** - всегда возвращает валидный JSON
2. **Понятные ошибки** - описание проблемы в поле `error`
3. **Graceful degradation** - частичные данные при возможности

### Типы ошибок:

| Ошибка                       | Причина                          | Решение                  |
| ---------------------------- | -------------------------------- | ------------------------ |
| `Invalid path`               | Пустой или null путь             | Проверить путь           |
| `Could not open file`        | Файл не существует или поврежден | Проверить файл           |
| `Could not find stream info` | Неподдерживаемый формат          | Конвертировать файл      |
| `No video stream found`      | Только аудио файл                | Использовать аудио плеер |

---

## 📊 Performance

### Benchmarks:

| Метод                    | Время    | Память |
| ------------------------ | -------- | ------ |
| `getVideoMetadata()`     | ~10-15ms | ~2MB   |
| `getVideoMetadataJson()` | ~10-15ms | ~2MB   |

**Примечание:** Время зависит от размера файла и формата.

---

## 🔄 Migration Guide

### From v1.0.4 to v1.0.5:

**Старый код (v1.0.4):**

```kotlin
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)
val width = metadata?.get("width") as? Int
val height = metadata?.get("height") as? Int
```

**Новый код (v1.0.5):**

```kotlin
// Вариант 1: HashMap (обратно совместимо)
val metadata = SmartFfmpegBridge.getVideoMetadata(videoPath)
val width = metadata?.get("width") as? Int
val fps = metadata?.get("fps") as? Double  // Новое поле!

// Вариант 2: JSON (рекомендуется)
val json = SmartFfmpegBridge.getVideoMetadataJson(videoPath)
val jsonObject = JSONObject(json)
if (jsonObject.getBoolean("success")) {
    val data = jsonObject.getJSONObject("data")
    val width = data.getInt("width")
    val fps = data.getDouble("fps")
}
```

---

## ✅ Testing

Запустите тесты:

```bash
./gradlew test
```

Новые тесты:

- `ExtendedMetadataTest` - проверка новых полей
- `JsonMetadataTest` - проверка JSON формата
- `SafeModeTest` - проверка обработки ошибок

---

## 📚 See Also

- [FFMPEG_CAPABILITIES.md](FFMPEG_CAPABILITIES.md) - Возможности FFmpeg
- [JSON_METADATA_PROPOSAL.md](JSON_METADATA_PROPOSAL.md) - Детали JSON API
- [USAGE.md](USAGE.md) - Общее использование

---

**Version:** 1.0.5  
**Date:** 2026-02-13  
**Status:** ✅ Ready for production
