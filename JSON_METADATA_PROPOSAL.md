# Предложение: Добавить JSON output для метаданных

## Текущая реализация

```kotlin
// Возвращает HashMap
val metadata: Map<String, Any>? = SmartFfmpegBridge.getVideoMetadata(videoPath)
```

## Предлагаемое улучшение

### Вариант 1: Добавить метод getVideoMetadataJson()

```kotlin
// Новый метод
val json: String? = SmartFfmpegBridge.getVideoMetadataJson(videoPath)

// Пример вывода:
{
  "format": {
    "filename": "/path/to/video.mp4",
    "format_name": "mov,mp4,m4a,3gp,3g2,mj2",
    "format_long_name": "QuickTime / MOV",
    "duration": "120.500000",
    "size": "52428800",
    "bit_rate": "3500000",
    "probe_score": 100
  },
  "streams": [
    {
      "index": 0,
      "codec_name": "h264",
      "codec_long_name": "H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10",
      "codec_type": "video",
      "width": 1920,
      "height": 1080,
      "pix_fmt": "yuv420p",
      "level": 40,
      "r_frame_rate": "30/1",
      "avg_frame_rate": "30/1",
      "time_base": "1/15360",
      "duration_ts": 1851392,
      "duration": "120.500000",
      "bit_rate": "3000000",
      "nb_frames": "3615"
    },
    {
      "index": 1,
      "codec_name": "aac",
      "codec_long_name": "AAC (Advanced Audio Coding)",
      "codec_type": "audio",
      "sample_rate": "48000",
      "channels": 2,
      "channel_layout": "stereo",
      "bits_per_sample": 0,
      "bit_rate": "128000"
    }
  ]
}
```

### Вариант 2: Расширенный метод с опциями

```kotlin
data class MetadataOptions(
    val includeStreams: Boolean = true,
    val includeFormat: Boolean = true,
    val includeChapters: Boolean = false,
    val prettyPrint: Boolean = true
)

val json = SmartFfmpegBridge.getVideoMetadataJson(
    videoPath = videoPath,
    options = MetadataOptions(prettyPrint = true)
)
```

## Реализация в JNI

### C код (ffmpeg_bridge_jni.c)

```c
#include <jansson.h>  // JSON library

JNIEXPORT jstring JNICALL
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadataJson(
    JNIEnv *env,
    jobject thiz,
    jstring videoPath
) {
    const char *path = (*env)->GetStringUTFChars(env, videoPath, NULL);

    AVFormatContext *formatCtx = NULL;
    if (avformat_open_input(&formatCtx, path, NULL, NULL) != 0) {
        return NULL;
    }

    avformat_find_stream_info(formatCtx, NULL);

    // Create JSON object
    json_t *root = json_object();

    // Add format info
    json_t *format = json_object();
    json_object_set_new(format, "filename", json_string(path));
    json_object_set_new(format, "format_name", json_string(formatCtx->iformat->name));
    json_object_set_new(format, "duration", json_real(formatCtx->duration / (double)AV_TIME_BASE));
    json_object_set_new(format, "size", json_integer(formatCtx->pb ? avio_size(formatCtx->pb) : 0));
    json_object_set_new(format, "bit_rate", json_integer(formatCtx->bit_rate));
    json_object_set(root, "format", format);

    // Add streams info
    json_t *streams = json_array();
    for (int i = 0; i < formatCtx->nb_streams; i++) {
        AVStream *stream = formatCtx->streams[i];
        AVCodecParameters *codecpar = stream->codecpar;

        json_t *stream_obj = json_object();
        json_object_set_new(stream_obj, "index", json_integer(i));

        AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
        if (codec) {
            json_object_set_new(stream_obj, "codec_name", json_string(codec->name));
            json_object_set_new(stream_obj, "codec_long_name", json_string(codec->long_name));
        }

        const char *codec_type = av_get_media_type_string(codecpar->codec_type);
        json_object_set_new(stream_obj, "codec_type", json_string(codec_type));

        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            json_object_set_new(stream_obj, "width", json_integer(codecpar->width));
            json_object_set_new(stream_obj, "height", json_integer(codecpar->height));
            json_object_set_new(stream_obj, "pix_fmt",
                json_string(av_get_pix_fmt_name(codecpar->format)));

            // Frame rate
            AVRational frame_rate = av_guess_frame_rate(formatCtx, stream, NULL);
            char fps_str[32];
            snprintf(fps_str, sizeof(fps_str), "%d/%d", frame_rate.num, frame_rate.den);
            json_object_set_new(stream_obj, "r_frame_rate", json_string(fps_str));
        }

        if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            json_object_set_new(stream_obj, "sample_rate", json_integer(codecpar->sample_rate));
            json_object_set_new(stream_obj, "channels", json_integer(codecpar->channels));
            json_object_set_new(stream_obj, "bit_rate", json_integer(codecpar->bit_rate));
        }

        json_array_append_new(streams, stream_obj);
    }
    json_object_set(root, "streams", streams);

    // Convert to string
    char *json_str = json_dumps(root, JSON_INDENT(2) | JSON_PRESERVE_ORDER);
    jstring result = (*env)->NewStringUTF(env, json_str);

    // Cleanup
    free(json_str);
    json_decref(root);
    avformat_close_input(&formatCtx);
    (*env)->ReleaseStringUTFChars(env, videoPath, path);

    return result;
}
```

### Kotlin API

```kotlin
object SmartFfmpegBridge {

    // Существующий метод (HashMap)
    @JvmStatic
    external fun getVideoMetadata(videoPath: String): Map<String, Any>?

    // Новый метод (JSON)
    @JvmStatic
    external fun getVideoMetadataJson(videoPath: String): String?

    // Удобный метод с парсингом
    @JvmStatic
    fun getVideoMetadataAsJson(videoPath: String): JSONObject? {
        val jsonString = getVideoMetadataJson(videoPath) ?: return null
        return try {
            JSONObject(jsonString)
        } catch (e: Exception) {
            null
        }
    }
}
```

## Использование

### Kotlin

```kotlin
// Вариант 1: Получить JSON строку
val json = SmartFfmpegBridge.getVideoMetadataJson(videoPath)
println(json)

// Вариант 2: Получить JSONObject
val metadata = SmartFfmpegBridge.getVideoMetadataAsJson(videoPath)
val width = metadata?.getJSONArray("streams")
    ?.getJSONObject(0)
    ?.getInt("width")

// Вариант 3: Использовать Gson/Moshi для десериализации
data class VideoMetadata(
    val format: FormatInfo,
    val streams: List<StreamInfo>
)

val gson = Gson()
val metadata = gson.fromJson(json, VideoMetadata::class.java)
```

### Flutter

```dart
// В Flutter плагине
final String jsonString = await platform.invokeMethod('getVideoMetadataJson', {
  'videoPath': videoPath,
});

final Map<String, dynamic> metadata = jsonDecode(jsonString);
print('Width: ${metadata['streams'][0]['width']}');
print('Codec: ${metadata['streams'][0]['codec_name']}');
```

## Преимущества JSON формата

1. **Стандартизация** - совместимость с ffprobe
2. **Полнота** - можно включить все поля
3. **Типобезопасность** - легко парсить в типизированные объекты
4. **Расширяемость** - легко добавлять новые поля
5. **Кроссплатформенность** - одинаковый формат для Android/iOS

## Зависимости

Нужно добавить JSON библиотеку в CMakeLists.txt:

```cmake
# Вариант 1: jansson (легковесная C библиотека)
find_package(jansson REQUIRED)
target_link_libraries(smart_ffmpeg jansson)

# Вариант 2: cJSON (еще проще)
add_library(cjson STATIC cJSON.c)
target_link_libraries(smart_ffmpeg cjson)

# Вариант 3: Использовать JNI для создания JSON (без C библиотек)
# Создавать JSON строку вручную в C коде
```

## Альтернатива: JSON без C библиотек

Можно создавать JSON вручную в C:

```c
char json[4096];
snprintf(json, sizeof(json),
    "{"
    "\"format\":{"
    "\"filename\":\"%s\","
    "\"duration\":%.2f,"
    "\"bit_rate\":%lld"
    "},"
    "\"streams\":["
    "{"
    "\"codec_name\":\"%s\","
    "\"width\":%d,"
    "\"height\":%d"
    "}"
    "]"
    "}",
    path,
    formatCtx->duration / (double)AV_TIME_BASE,
    (long long)formatCtx->bit_rate,
    codec->name,
    codecpar->width,
    codecpar->height
);
```

## Рекомендация

Я рекомендую **Вариант 1** с использованием `jansson` или `cJSON`:

- ✅ Чистый код
- ✅ Автоматическое экранирование
- ✅ Легко расширять
- ✅ Меньше ошибок

Хотите, чтобы я реализовал это?
