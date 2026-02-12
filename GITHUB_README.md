# Smart FFmpeg Android

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![GitHub release](https://img.shields.io/github/release/Daronec/smart-ffmpeg-android.svg)](https://github.com/Daronec/smart-ffmpeg-android/releases)

Android библиотека для воспроизведения видео и работы с медиафайлами на основе FFmpeg.

## Возможности

- 🎬 Воспроизведение видео через нативный FFmpeg плеер
- 🖼️ Извлечение thumbnail из видео
- 📊 Получение метаданных видео (duration, width, height)
- ⚡ Поддержка аппаратного ускорения (MediaCodec)
- 🎵 Синхронизация аудио/видео
- 🎯 Seek с точностью до кадра
- ⏩ Управление скоростью воспроизведения (0.5x - 3.0x)
- 📦 Поддержка различных форматов (MP4, AVI, FLV, MKV, WebM и др.)

## Установка

### 1. Добавьте репозиторий

В `settings.gradle`:

```groovy
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven {
            url = uri("https://maven.pkg.github.com/Daronec/smart-ffmpeg-android")
            credentials {
                username = project.findProperty("gpr.user") ?: System.getenv("GPR_USER")
                password = project.findProperty("gpr.key") ?: System.getenv("GPR_KEY")
            }
        }
    }
}
```

### 2. Настройте credentials

Создайте `~/.gradle/gradle.properties`:

```properties
gpr.user=YOUR_GITHUB_USERNAME
gpr.key=YOUR_GITHUB_TOKEN
```

[Как создать GitHub Token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/creating-a-personal-access-token)

### 3. Добавьте зависимость

В `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg-android:1.0.0'
}
```

## Быстрый старт

### Воспроизведение видео

```kotlin
class VideoPlayerActivity : AppCompatActivity() {
    private lateinit var player: SmartFFmpegPlayer

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val surfaceView = SurfaceView(this)
        setContentView(surfaceView)

        player = SmartFFmpegPlayer()
        player.setEventCallback(object : SmartFFmpegPlayer.EventCallback {
            override fun onPrepared(hasAudio: Boolean, durationMs: Long) {
                player.play()
            }

            override fun onError(message: String) {
                Log.e("Player", "Error: $message")
            }

            // ... другие callbacks
        })

        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                player.setSurface(holder.surface)
                player.prepare("/path/to/video.mp4")
            }

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                player.setSurface(null)
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
        })
    }

    override fun onDestroy() {
        super.onDestroy()
        player.release()
    }
}
```

### Извлечение thumbnail

```kotlin
// Получить thumbnail на 5-й секунде
val thumbnailData = SmartFfmpegBridge.extractThumbnail(
    videoPath = "/path/to/video.mp4",
    timeMs = 5000,
    width = 320,
    height = 180
)

if (thumbnailData != null) {
    val bitmap = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
    bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(thumbnailData))
    imageView.setImageBitmap(bitmap)
}
```

### Получение метаданных

```kotlin
// Длительность
val duration = SmartFfmpegBridge.getVideoDuration("/path/to/video.mp4")

// Метаданные
val metadata = SmartFfmpegBridge.getVideoMetadata("/path/to/video.mp4")
val width = metadata?.get("width") as? Int
val height = metadata?.get("height") as? Int

// Версия FFmpeg
val version = SmartFfmpegBridge.getFFmpegVersion()
```

## API

### SmartFFmpegPlayer

| Метод                                      | Описание                            |
| ------------------------------------------ | ----------------------------------- |
| `prepare(path: String)`                    | Подготовить видео к воспроизведению |
| `play()`                                   | Начать воспроизведение              |
| `pause()`                                  | Приостановить воспроизведение       |
| `seekTo(positionMs: Long, exact: Boolean)` | Перемотать на позицию               |
| `setSpeed(speed: Float)`                   | Установить скорость (0.5 - 3.0)     |
| `getPosition()`                            | Получить текущую позицию            |
| `getDuration()`                            | Получить длительность               |
| `release()`                                | Освободить ресурсы                  |

### SmartFfmpegBridge

| Метод                                           | Описание               |
| ----------------------------------------------- | ---------------------- |
| `extractThumbnail(path, timeMs, width, height)` | Извлечь thumbnail      |
| `getVideoDuration(path)`                        | Получить длительность  |
| `getVideoMetadata(path)`                        | Получить метаданные    |
| `getFFmpegVersion()`                            | Получить версию FFmpeg |

## Требования

- Android API 21+ (Android 5.0 Lollipop)
- Поддержка архитектур: arm64-v8a, armeabi-v7a

## Документация

- [Полная документация](README.md)
- [Руководство по интеграции](INTEGRATION_GUIDE.md)
- [Структура проекта](STRUCTURE.md)
- [Публикация библиотеки](PUBLISH.md)

## Сборка из исходников

```bash
# Клонировать репозиторий
git clone https://github.com/Daronec/smart-ffmpeg-android.git
cd smart-ffmpeg-android

# Собрать библиотеку
./gradlew assembleRelease

# Опубликовать в локальный Maven
./gradlew publishToMavenLocal
```

## Лицензия

MIT License - см. [LICENSE](LICENSE)

## Автор

[Daronec](https://github.com/Daronec)

## Поддержка

Если у вас возникли вопросы или проблемы:

- [Создайте Issue](https://github.com/Daronec/smart-ffmpeg-android/issues)
- [Обсуждения](https://github.com/Daronec/smart-ffmpeg-android/discussions)

## Благодарности

Основано на [FFmpeg](https://ffmpeg.org/) - мощной мультимедийной библиотеке.
