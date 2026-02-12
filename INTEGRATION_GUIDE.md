# Руководство по интеграции Smart FFmpeg Android

## Структура проекта

```
smart-ffmpeg-android/
├── src/
│   └── main/
│       ├── cpp/
│       │   ├── CMakeLists.txt
│       │   ├── jni_bridge.c
│       │   ├── ffmpeg_bridge.cpp
│       │   └── native_media_engine/  # Нативный движок FFmpeg
│       └── kotlin/com/smartmedia/ffmpeg/
│           └── SmartFFmpegPlayer.kt
├── build.gradle
└── README.md
```

## Что уже сделано

✅ Добавлен нативный движок FFmpeg в `native_media_engine/`
✅ Создана структура Android библиотеки
✅ Настроен CMake для сборки нативного кода
✅ Создан JNI bridge для связи Kotlin ↔ C
✅ Создан Kotlin API для удобного использования

## Что нужно доработать

### 1. Проверить зависимости в нативном коде

Ваш нативный движок использует следующие модули:

- `audio_renderer.h` / `audio_renderer.c`
- `video_renderer.h` / `video_renderer.c`
- `subtitle_manager.h` / `subtitle_manager.c`
- `avsync_gate.h` / `avsync_gate.c`
- `video_render_gl.h` / `video_render_gl.c`

Убедитесь, что все эти файлы присутствуют в `src/native_media_engine/ffmpeg_player/`.

### 2. Добавить недостающие реализации

В `jni_bridge.c` есть заглушка для `nativeSetSurface`:

```c
JNIEXPORT void JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativeSetSurface(JNIEnv *env, jobject thiz,
                                                               jlong handle, jobject surface) {
    PlayerContext *ctx = (PlayerContext*)handle;
    if (ctx == NULL) {
        LOGE("nativeSetSurface: Invalid handle");
        return;
    }

    ANativeWindow *window = NULL;
    if (surface != NULL) {
        window = ANativeWindow_fromSurface(env, surface);
    }

    // TODO: Вызвать функцию из video_renderer для установки surface
    // Например: video_renderer_set_surface(ctx->video, window);
}
```

Нужно добавить вызов функции из вашего `video_renderer` для установки surface.

### 3. Добавить armeabi-v7a библиотеки

Сейчас в `src/native_media_engine/jniLibs/` есть только `arm64-v8a`.
Если нужна поддержка 32-битных устройств, добавьте `armeabi-v7a/` с соответствующими библиотеками.

### 4. Проверить сборку

```bash
# Очистить предыдущую сборку
./gradlew clean

# Собрать библиотеку
./gradlew assembleRelease

# Проверить, что .so файлы созданы
ls build/intermediates/cmake/release/obj/arm64-v8a/
```

### 5. Тестирование

Создайте тестовое Android приложение:

```kotlin
// app/build.gradle
dependencies {
    implementation project(':smart-ffmpeg-android')
}

// MainActivity.kt
class MainActivity : AppCompatActivity() {
    private lateinit var player: SmartFFmpegPlayer

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        player = SmartFFmpegPlayer()
        player.setEventCallback(object : SmartFFmpegPlayer.EventCallback {
            override fun onPrepared(hasAudio: Boolean, durationMs: Long) {
                Log.d("Test", "Video prepared: $durationMs ms")
                player.play()
            }

            override fun onError(message: String) {
                Log.e("Test", "Error: $message")
            }

            // ... остальные callbacks
        })

        // Тест с локальным файлом
        val testVideo = "/sdcard/test.mp4"
        player.prepare(testVideo)
    }
}
```

## Возможные проблемы

### Проблема: Ошибка линковки FFmpeg библиотек

Решение: Убедитесь, что пути в `CMakeLists.txt` правильные:

```cmake
target_link_libraries(smart_ffmpeg
    ${NATIVE_ENGINE_DIR}/jniLibs/${ANDROID_ABI}/libavcodec.so
    # ...
)
```

### Проблема: Не найдены заголовочные файлы

Решение: Проверьте `include_directories` в `CMakeLists.txt`:

```cmake
include_directories(
    ${NATIVE_ENGINE_DIR}/include
    ${NATIVE_ENGINE_DIR}/ffmpeg_player
)
```

### Проблема: JNI методы не найдены

Решение: Убедитесь, что имена методов в `jni_bridge.c` совпадают с package и классом:

```c
// Для com.smartmedia.ffmpeg.SmartFFmpegPlayer
JNIEXPORT jlong JNICALL
Java_com_smartmedia_ffmpeg_SmartFFmpegPlayer_nativePrepare(...)
```

## Следующие шаги

1. Проверьте наличие всех файлов в `src/native_media_engine/ffmpeg_player/`
2. Доработайте `nativeSetSurface` для установки surface в video renderer
3. Соберите проект: `./gradlew assembleRelease`
4. Создайте тестовое приложение для проверки
5. Протестируйте на реальном устройстве

## Дополнительные возможности

После базовой интеграции можно добавить:

- Поддержку субтитров (уже есть в нативном коде)
- Thumbnail extraction
- Metadata extraction
- Hardware acceleration (MediaCodec)
- Streaming support (HTTP/RTSP)

## Контакты

Если возникнут вопросы по интеграции, проверьте:

- Логи сборки CMake
- Logcat для runtime ошибок
- Документацию FFmpeg
