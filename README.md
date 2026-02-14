# Smart FFmpeg Android

[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL%20v2.1-blue.svg)](https://www.gnu.org/licenses/lgpl-2.1)
[![GitHub release](https://img.shields.io/github/v/release/Daronec/smart-ffmpeg-android)](https://github.com/Daronec/smart-ffmpeg-android/releases)
[![JitPack](https://jitpack.io/v/Daronec/smart-ffmpeg-android.svg)](https://jitpack.io/#Daronec/smart-ffmpeg-android)
[![GitHub Packages](https://img.shields.io/badge/GitHub%20Packages-1.0.5-blue)](https://github.com/Daronec/smart-ffmpeg-android/packages)
[![Build](https://github.com/Daronec/smart-ffmpeg-android/workflows/Build/badge.svg)](https://github.com/Daronec/smart-ffmpeg-android/actions)
[![Platform](https://img.shields.io/badge/platform-Android-green.svg)](https://github.com/Daronec/smart-ffmpeg-android)

Мощная Android библиотека для воспроизведения видео и работы с медиафайлами на основе FFmpeg.

[English](#english) | [Русский](#russian)

---

## <a name="russian"></a>🇷🇺 Русский

### ✨ Возможности

- 🎬 **Воспроизведение видео** - полнофункциональный нативный плеер на FFmpeg
- 🖼️ **Извлечение thumbnail** - быстрое получение превью из видео
- 📊 **Метаданные** - информация о видео (размер, длительность, кодек)
- ⚡ **Аппаратное ускорение** - поддержка MediaCodec для H.264/HEVC
- 🎵 **Синхронизация A/V** - точная синхронизация аудио и видео
- 🎯 **Точный seek** - перемотка с точностью до кадра
- ⏩ **Скорость воспроизведения** - от 0.5x до 3.0x
- 📦 **Множество форматов** - MP4, AVI, FLV, MKV, WebM и другие

### 🚀 Быстрый старт

#### Установка

**Вариант 1: JitPack (рекомендуется)**

[![JitPack](https://jitpack.io/v/Daronec/smart-ffmpeg-android.svg)](https://jitpack.io/#Daronec/smart-ffmpeg-android)

1. Добавьте репозиторий JitPack в `settings.gradle`:

```groovy
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url 'https://jitpack.io' }
    }
}
```

2. Добавьте зависимость в `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.5'
}
```

**Готово!** Никаких GitHub credentials не требуется.

🔗 **Ссылки:**

- 📦 [JitPack Repository](https://jitpack.io/#Daronec/smart-ffmpeg-android)
- 📋 [Все версии на JitPack](https://jitpack.io/#Daronec/smart-ffmpeg-android)

---

**Вариант 2: GitHub Packages**

[![GitHub Packages](https://img.shields.io/badge/GitHub%20Packages-1.0.5-blue)](https://github.com/Daronec/smart-ffmpeg-android/packages)

1. Добавьте репозиторий в `settings.gradle`:

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

2. Настройте credentials в `~/.gradle/gradle.properties`:

```properties
gpr.user=YOUR_GITHUB_USERNAME
gpr.key=YOUR_GITHUB_TOKEN
```

> 💡 [Как создать GitHub Token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/creating-a-personal-access-token) (требуется: `read:packages`)

3. Добавьте зависимость в `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.5'
}
```

🔗 **Ссылки:**

- 📦 [GitHub Packages](https://github.com/Daronec/smart-ffmpeg-android/packages)
- 📋 [Все релизы](https://github.com/Daronec/smart-ffmpeg-android/releases)

#### Воспроизведение видео

```kotlin
import com.smartmedia.ffmpeg.SmartFFmpegPlayer
import android.view.SurfaceView

class VideoPlayerActivity : AppCompatActivity() {
    private lateinit var player: SmartFFmpegPlayer
    private lateinit var surfaceView: SurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        surfaceView = SurfaceView(this)
        setContentView(surfaceView)

        // Создать плеер
        player = SmartFFmpegPlayer()

        // Установить callbacks
        player.setEventCallback(object : SmartFFmpegPlayer.EventCallback {
            override fun onPrepared(hasAudio: Boolean, durationMs: Long) {
                Log.d("Player", "Готов к воспроизведению: $durationMs ms")
                player.play()
            }

            override fun onFirstFrame() {
                Log.d("Player", "Первый кадр отрисован")
            }

            override fun onPosition(positionMs: Long) {
                // Обновление позиции каждые 100ms
            }

            override fun onEnded() {
                Log.d("Player", "Воспроизведение завершено")
            }

            override fun onError(message: String) {
                Log.e("Player", "Ошибка: $message")
            }

            override fun onSurfaceReady() {}
            override fun onFirstFrameAfterSeek() {}
            override fun onAudioStateChanged(state: String) {}
        })

        // Настроить Surface
        surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) {
                player.setSurface(holder.surface)

                // Подготовить видео
                val videoPath = "/sdcard/video.mp4"
                if (player.prepare(videoPath)) {
                    Log.d("Player", "Подготовка началась")
                }
            }

            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

            override fun surfaceDestroyed(holder: SurfaceHolder) {
                player.setSurface(null)
            }
        })
    }

    override fun onDestroy() {
        super.onDestroy()
        player.release()
    }

    // Управление воспроизведением
    fun pauseVideo() = player.pause()
    fun resumeVideo() = player.play()
    fun seekTo(positionMs: Long) = player.seekTo(positionMs, exact = false)
    fun setSpeed(speed: Float) = player.setSpeed(speed) // 0.5x - 3.0x
}
```

#### Извлечение thumbnail

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge
import android.graphics.Bitmap
import java.nio.ByteBuffer

// Извлечь thumbnail на 5-й секунде
val thumbnailData = SmartFfmpegBridge.extractThumbnail(
    videoPath = "/sdcard/video.mp4",
    timeMs = 5000,  // 5 секунд
    width = 320,
    height = 180
)

if (thumbnailData != null) {
    val bitmap = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
    bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(thumbnailData))
    imageView.setImageBitmap(bitmap)
}
```

#### Получение метаданных

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge

// Получить длительность видео
val durationMs = SmartFfmpegBridge.getVideoDuration("/sdcard/video.mp4")
Log.d("Video", "Длительность: ${durationMs / 1000} секунд")

// Получить метаданные
val metadata = SmartFfmpegBridge.getVideoMetadata("/sdcard/video.mp4")
metadata?.let {
    Log.d("Video", "Размер: ${it.width}x${it.height}")
    Log.d("Video", "Длительность: ${it.durationMs} ms")
    Log.d("Video", "Кодек: ${it.codecName}")
}

// Версия FFmpeg
val version = SmartFfmpegBridge.getFFmpegVersion()
Log.d("FFmpeg", "Версия: $version")
```

### 📚 API Reference

#### SmartFFmpegPlayer

Основной класс для воспроизведения видео.

**Методы:**

- `prepare(videoPath: String): Boolean` - подготовить видео к воспроизведению
- `play()` - начать/возобновить воспроизведение
- `pause()` - приостановить воспроизведение
- `stop()` - остановить воспроизведение
- `release()` - освободить ресурсы
- `seekTo(positionMs: Long, exact: Boolean)` - перемотать на позицию
- `setSpeed(speed: Float)` - установить скорость (0.5 - 3.0)
- `setSurface(surface: Surface?)` - установить Surface для рендеринга
- `setEventCallback(callback: EventCallback)` - установить обработчик событий
- `getCurrentPosition(): Long` - получить текущую позицию в ms
- `getDuration(): Long` - получить длительность в ms

**События (EventCallback):**

- `onPrepared(hasAudio: Boolean, durationMs: Long)` - видео готово
- `onSurfaceReady()` - Surface готов
- `onFirstFrame()` - первый кадр отрисован
- `onFirstFrameAfterSeek()` - первый кадр после seek
- `onPosition(positionMs: Long)` - обновление позиции
- `onEnded()` - воспроизведение завершено
- `onError(message: String)` - ошибка
- `onAudioStateChanged(state: String)` - изменение состояния аудио

#### SmartFfmpegBridge

Утилиты для работы с видео.

**Методы:**

- `extractThumbnail(videoPath: String, timeMs: Long, width: Int, height: Int): ByteArray?` - извлечь thumbnail
- `getVideoDuration(videoPath: String): Long` - получить длительность
- `getVideoMetadata(videoPath: String): VideoMetadata?` - получить метаданные
- `getFFmpegVersion(): String` - версия FFmpeg

### 🏗️ Архитектура

```
src/main/
├── cpp/
│   ├── CMakeLists.txt                    # CMake конфигурация
│   └── native_media_engine/              # Нативный движок
│       ├── ffmpeg_player/                # Плеер (50+ файлов)
│       │   ├── ffmpeg_player.c           # Основной плеер
│       │   ├── audio_renderer.c          # Аудио рендеринг
│       │   ├── video_renderer.c          # Видео рендеринг
│       │   ├── avsync.c                  # A/V синхронизация
│       │   ├── packet_queue.c            # Очередь пакетов
│       │   ├── frame_queue.c             # Очередь кадров
│       │   └── ...
│       ├── include/                      # FFmpeg заголовки
│       └── jniLibs/arm64-v8a/           # FFmpeg библиотеки (7 .so)
│           ├── libavcodec.so
│           ├── libavformat.so
│           ├── libavutil.so
│           ├── libswresample.so
│           ├── libswscale.so
│           ├── libavfilter.so
│           └── libavdevice.so
├── kotlin/com/smartmedia/ffmpeg/
│   ├── SmartFFmpegPlayer.kt              # API плеера
│   └── SmartFfmpegBridge.kt              # API утилит
└── AndroidManifest.xml
```

### 🔧 Сборка из исходников

#### Требования

- Android Studio Arctic Fox или новее
- Android NDK r21+
- CMake 3.22.1+
- Gradle 8.0+
- Java 11+

#### Сборка библиотеки

```bash
# Клонировать репозиторий
git clone https://github.com/Daronec/smart-ffmpeg-android.git
cd smart-ffmpeg-android

# Собрать release версию
./gradlew clean assembleRelease

# AAR файл будет в:
# build/outputs/aar/smart-ffmpeg-android-release.aar
```

### 📋 Требования

- **Android API**: 21+ (Android 5.0 Lollipop)
- **Архитектуры**: arm64-v8a
- **Размер**: ~7 MB (AAR)

### 🔒 Безопасность

Если вы обнаружили уязвимость, пожалуйста, сообщите об этом через [Security Policy](SECURITY.md).

### 📝 Changelog

См. [CHANGELOG.md](CHANGELOG.md) для истории изменений.

### 📄 Лицензия

LGPL 2.1 - см. [LICENSE](LICENSE)

Основано на [FFmpeg](https://ffmpeg.org/) (LGPL 2.1)

---

## <a name="english"></a>🇬🇧 English

### ✨ Features

- 🎬 **Video Playback** - Full-featured native FFmpeg player
- 🖼️ **Thumbnail Extraction** - Fast video preview generation
- 📊 **Metadata** - Video information (size, duration, codec)
- ⚡ **Hardware Acceleration** - MediaCodec support for H.264/HEVC
- 🎵 **A/V Sync** - Precise audio/video synchronization
- 🎯 **Frame-Accurate Seeking** - Precise frame navigation
- ⏩ **Playback Speed** - 0.5x to 3.0x speed control
- 📦 **Multiple Formats** - MP4, AVI, FLV, MKV, WebM, and more

### 🚀 Quick Start

#### Installation

**Option 1: JitPack (Recommended)**

[![JitPack](https://jitpack.io/v/Daronec/smart-ffmpeg-android.svg)](https://jitpack.io/#Daronec/smart-ffmpeg-android)

1. Add JitPack repository to `settings.gradle`:

```groovy
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url 'https://jitpack.io' }
    }
}
```

2. Add dependency to `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.5'
}
```

**Done!** No GitHub credentials required.

🔗 **Links:**

- 📦 [JitPack Repository](https://jitpack.io/#Daronec/smart-ffmpeg-android)
- 📋 [All versions on JitPack](https://jitpack.io/#Daronec/smart-ffmpeg-android)

---

**Option 2: GitHub Packages**

[![GitHub Packages](https://img.shields.io/badge/GitHub%20Packages-1.0.5-blue)](https://github.com/Daronec/smart-ffmpeg-android/packages)

**Option 2: GitHub Packages**

[![GitHub Packages](https://img.shields.io/badge/GitHub%20Packages-1.0.5-blue)](https://github.com/Daronec/smart-ffmpeg-android/packages)

1. Add repository to `settings.gradle`:

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

2. Configure credentials in `~/.gradle/gradle.properties`:

```properties
gpr.user=YOUR_GITHUB_USERNAME
gpr.key=YOUR_GITHUB_TOKEN
```

> 💡 [How to create GitHub Token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/creating-a-personal-access-token) (requires: `read:packages`)

3. Add dependency to `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.5'
}
```

🔗 **Links:**

- 📦 [GitHub Packages](https://github.com/Daronec/smart-ffmpeg-android/packages)
- 📋 [All releases](https://github.com/Daronec/smart-ffmpeg-android/releases)

#### Video Playback

```kotlin
import com.smartmedia.ffmpeg.SmartFFmpegPlayer

val player = SmartFFmpegPlayer()

player.setEventCallback(object : SmartFFmpegPlayer.EventCallback {
    override fun onPrepared(hasAudio: Boolean, durationMs: Long) {
        Log.d("Player", "Ready to play: $durationMs ms")
        player.play()
    }

    override fun onError(message: String) {
        Log.e("Player", "Error: $message")
    }

    // ... other callbacks
})

surfaceView.holder.addCallback(object : SurfaceHolder.Callback {
    override fun surfaceCreated(holder: SurfaceHolder) {
        player.setSurface(holder.surface)
        player.prepare("/path/to/video.mp4")
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        player.release()
    }
})
```

#### Thumbnail Extraction

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge

val thumbnailData = SmartFfmpegBridge.extractThumbnail(
    videoPath = "/path/to/video.mp4",
    timeMs = 5000,  // 5 seconds
    width = 320,
    height = 180
)

if (thumbnailData != null) {
    val bitmap = Bitmap.createBitmap(320, 180, Bitmap.Config.ARGB_8888)
    bitmap.copyPixelsFromBuffer(ByteBuffer.wrap(thumbnailData))
    imageView.setImageBitmap(bitmap)
}
```

### 📚 Documentation

- 📖 [Integration Guide](INTEGRATION_GUIDE.md)
- 🏗️ [Project Structure](STRUCTURE.md)
- 📦 [Publishing Guide](PUBLISH.md)
- 🔒 [Security Policy](SECURITY.md)
- 📝 [Changelog](CHANGELOG.md)

### 📋 Requirements

- **Android API**: 21+ (Android 5.0 Lollipop)
- **Architectures**: arm64-v8a
- **Size**: ~7 MB (AAR)

### 🔧 Building from Source

```bash
git clone https://github.com/Daronec/smart-ffmpeg-android.git
cd smart-ffmpeg-android
./gradlew clean assembleRelease
```

### 📄 License

LGPL 2.1 - see [LICENSE](LICENSE)

Based on [FFmpeg](https://ffmpeg.org/) (LGPL 2.1)

---

## 🤝 Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## 📞 Support

- 🐛 [Report Issues](https://github.com/Daronec/smart-ffmpeg-android/issues)
- 💬 [Discussions](https://github.com/Daronec/smart-ffmpeg-android/discussions)

## 👨‍💻 Author

[Daronec](https://github.com/Daronec)

## ⭐ Star History

If you find this project useful, please consider giving it a star!

---

Made with ❤️ using [FFmpeg](https://ffmpeg.org/)
