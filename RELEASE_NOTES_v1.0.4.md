# Release Notes - smart-ffmpeg-android v1.0.4

## 🎉 Основные изменения

Версия 1.0.4 включает полную реализацию JNI методов для работы с видео через FFmpeg.

## ✨ Новые возможности

### 1. Полная реализация JNI методов

Все методы `SmartFfmpegBridge` теперь полностью реализованы в нативном C коде:

- ✅ **extractThumbnail** - Извлечение миниатюры из видео в формате RGBA
- ✅ **getVideoDuration** - Получение длительности видео в миллисекундах
- ✅ **getVideoMetadata** - Получение метаданных (разрешение, кодек, битрейт)
- ✅ **getFFmpegVersion** - Получение версии FFmpeg

### 2. Исправлена архитектура Kotlin класса

- Изменен `SmartFfmpegBridge` с `class` на `object` (Kotlin singleton)
- Убран `companion object` для правильных JNI имен методов
- Исправлены JNI сигнатуры (убран `_00024Companion`)

### 3. Улучшенная обработка ошибок

- Добавлено логирование всех операций
- Корректная обработка ошибок FFmpeg
- Правильное освобождение памяти

## 🔧 Технические детали

### Реализованные JNI методы

```c
// 1. Извлечение миниатюры (1404 bytes)
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_extractThumbnail

// 2. Получение длительности (432 bytes)
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoDuration

// 3. Получение метаданных (1232 bytes)
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getVideoMetadata

// 4. Получение версии FFmpeg (80 bytes)
Java_com_smartmedia_ffmpeg_SmartFfmpegBridge_getFFmpegVersion
```

### Размер библиотеки

- **libsmart_ffmpeg.so**: 208.81 KB (213,824 bytes)
- **Архитектура**: arm64-v8a
- **Формат вывода**: RGBA (4 байта на пиксель)

## 📦 Установка

### Gradle

```kotlin
dependencies {
    implementation("com.smartmedia:smart-ffmpeg-android:1.0.4")
}
```

### Maven

```xml
<dependency>
    <groupId>com.smartmedia</groupId>
    <artifactId>smart-ffmpeg-android</artifactId>
    <version>1.0.4</version>
</dependency>
```

## 🚀 Использование

### Kotlin

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge

// Получить версию FFmpeg
val version = SmartFfmpegBridge.getFFmpegVersion()

// Получить длительность видео
val duration = SmartFfmpegBridge.getVideoDuration("/path/to/video.mp4")

// Получить метаданные
val metadata = SmartFfmpegBridge.getVideoMetadata("/path/to/video.mp4")

// Извлечь миниатюру
val thumbnail = SmartFfmpegBridge.extractThumbnail(
    videoPath = "/path/to/video.mp4",
    timeMs = 5000L,
    width = 640,
    height = 360
)
```

## 🐛 Исправленные ошибки

1. **UnsatisfiedLinkError**: Исправлена ошибка "No implementation found for extractThumbnail"
2. **JNI naming**: Исправлены имена JNI методов (убран `_00024Companion`)
3. **Memory leaks**: Добавлено корректное освобождение памяти FFmpeg
4. **Error handling**: Улучшена обработка ошибок при открытии видео файлов

## 📋 Требования

- **Minimum SDK**: 26 (Android 8.0)
- **Target SDK**: 34
- **Архитектура**: arm64-v8a
- **NDK**: 25.1.8937393 или выше

## 🔍 Тестирование

Все тесты проходят успешно:

```bash
./gradlew test
# BUILD SUCCESSFUL

./gradlew assembleRelease
# BUILD SUCCESSFUL
```

### Проверка JNI методов

```bash
llvm-readelf -s libsmart_ffmpeg.so | grep Java_com_smartmedia_ffmpeg
# ✅ Все 4 метода найдены
```

## 📚 Документация

- [USAGE.md](USAGE.md) - Руководство по использованию
- [FLUTTER_INTEGRATION.md](FLUTTER_INTEGRATION.md) - Интеграция с Flutter
- [TEST_JNI_METHODS.md](TEST_JNI_METHODS.md) - Тестирование JNI методов

## 🔗 Ссылки

- **GitHub**: https://github.com/Daronec/smart-ffmpeg-android
- **Issues**: https://github.com/Daronec/smart-ffmpeg-android/issues
- **Releases**: https://github.com/Daronec/smart-ffmpeg-android/releases/tag/v1.0.4

## 📝 Changelog

### v1.0.4 (2026-02-13)

- ✅ Реализованы все JNI методы в C коде
- ✅ Изменен SmartFfmpegBridge на Kotlin object
- ✅ Исправлены JNI имена методов
- ✅ Добавлено логирование и обработка ошибок
- ✅ Обновлена документация
- ✅ Добавлены руководства по интеграции

### v1.0.3 (2026-02-12)

- Исправлена загрузка библиотеки (smart_ffmpeg вместо ffmpeg_bridge)
- Добавлены комплексные тесты

### v1.0.2 (2026-02-12)

- Обновлена версия после исправлений

### v1.0.1 (2026-02-12)

- Первая рабочая версия с исправленной загрузкой библиотеки

### v1.0.0 (2026-02-12)

- Первый релиз

## 👥 Авторы

- Daronec - https://github.com/Daronec

## 📄 Лицензия

[Укажите вашу лицензию]

---

**Примечание**: Эта версия полностью готова к использованию в production. Все JNI методы реализованы и протестированы.
