# Smart FFmpeg Android

[![License: LGPL v2.1](https://img.shields.io/badge/License-LGPL%20v2.1-blue.svg)](https://www.gnu.org/licenses/lgpl-2.1)
[![GitHub release](https://img.shields.io/github/v/release/Daronec/smart-ffmpeg-android)](https://github.com/Daronec/smart-ffmpeg-android/releases)
[![Build](https://github.com/Daronec/smart-ffmpeg-android/workflows/Build/badge.svg)](https://github.com/Daronec/smart-ffmpeg-android/actions)

Мощная Android библиотека для воспроизведения видео и работы с медиафайлами на основе FFmpeg.

[English](#english) | [Русский](#russian)

---

## <a name="russian"></a>🇷🇺 Русский

### Возможности

- 🎬 **Воспроизведение видео** - полнофункциональный нативный плеер на FFmpeg
- 🖼️ **Извлечение thumbnail** - быстрое получение превью из видео
- 📊 **Метаданные** - информация о видео (размер, длительность, кодек)
- ⚡ **Аппаратное ускорение** - поддержка MediaCodec для H.264/HEVC
- 🎵 **Синхронизация A/V** - точная синхронизация аудио и видео
- 🎯 **Точный seek** - перемотка с точностью до кадра
- ⏩ **Скорость воспроизведения** - от 0.5x до 3.0x
- 📦 **Множество форматов** - MP4, AVI, FLV, MKV, WebM и другие

### Быстрый старт

#### Установка

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

[Как создать GitHub Token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/creating-a-personal-access-token)

3. Добавьте зависимость в `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg-android:1.0.0'
}
```

#### Воспроизведение видео

```kotlin
val player = SmartFFmpegPlayer()

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
        player.release()
    }
})
```

#### Извлечение thumbnail

```kotlin
val thumbnailData = SmartFfmpegBridge.extractThumbnail(
    videoPath = "/path/to/video.mp4",
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

### Документация

- 📖 [Полная документация](README.md)
- 🔧 [Руководство по интеграции](INTEGRATION_GUIDE.md)
- 🏗️ [Структура проекта](STRUCTURE.md)
- 📦 [Публикация библиотеки](PUBLISH.md)
- 🔒 [Безопасность](SECURITY.md)
- 📝 [Changelog](CHANGELOG.md)

### Требования

- Android API 21+ (Android 5.0 Lollipop)
- Архитектуры: arm64-v8a

### Лицензия

LGPL 2.1 - см. [LICENSE](LICENSE)

Основано на [FFmpeg](https://ffmpeg.org/) (LGPL 2.1)

---

## <a name="english"></a>🇬🇧 English

### Features

- 🎬 **Video Playback** - Full-featured native FFmpeg player
- 🖼️ **Thumbnail Extraction** - Fast video preview generation
- 📊 **Metadata** - Video information (size, duration, codec)
- ⚡ **Hardware Acceleration** - MediaCodec support for H.264/HEVC
- 🎵 **A/V Sync** - Precise audio/video synchronization
- 🎯 **Frame-Accurate Seeking** - Precise frame navigation
- ⏩ **Playback Speed** - 0.5x to 3.0x speed control
- 📦 **Multiple Formats** - MP4, AVI, FLV, MKV, WebM, and more

### Quick Start

#### Installation

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

[How to create GitHub Token](https://docs.github.com/en/authentication/keeping-your-account-and-data-secure/creating-a-personal-access-token)

3. Add dependency to `app/build.gradle`:

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg-android:1.0.0'
}
```

#### Video Playback

```kotlin
val player = SmartFFmpegPlayer()

player.setEventCallback(object : SmartFFmpegPlayer.EventCallback {
    override fun onPrepared(hasAudio: Boolean, durationMs: Long) {
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

### Documentation

- 📖 [Full Documentation](README.md)
- 🔧 [Integration Guide](INTEGRATION_GUIDE.md)
- 🏗️ [Project Structure](STRUCTURE.md)
- 📦 [Publishing Guide](PUBLISH.md)
- 🔒 [Security](SECURITY.md)
- 📝 [Changelog](CHANGELOG.md)

### Requirements

- Android API 21+ (Android 5.0 Lollipop)
- Architectures: arm64-v8a

### License

LGPL 2.1 - see [LICENSE](LICENSE)

Based on [FFmpeg](https://ffmpeg.org/) (LGPL 2.1)

---

## 🤝 Contributing

Contributions are welcome! Please read our contributing guidelines before submitting PRs.

## 📞 Support

- 🐛 [Report Issues](https://github.com/Daronec/smart-ffmpeg-android/issues)
- 💬 [Discussions](https://github.com/Daronec/smart-ffmpeg-android/discussions)
- 📧 Contact: [Your Email]

## 👨‍💻 Author

[Daronec](https://github.com/Daronec)

## ⭐ Star History

If you find this project useful, please consider giving it a star!

---

Made with ❤️ using [FFmpeg](https://ffmpeg.org/)
