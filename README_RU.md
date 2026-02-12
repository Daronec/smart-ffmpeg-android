# Smart FFmpeg Android Library

Отдельная Android библиотека с FFmpeg для извлечения обложек видео.

## 🎯 Что это?

Это **отдельный** Android Library проект, который:

- Содержит FFmpeg (только LGPL)
- Собирается в .aar файл (~12-15MB)
- Публикуется в GitHub Packages
- Используется Flutter плагином как зависимость

## ✨ Преимущества

- ✅ Чистое разделение ответственности
- ✅ Независимое версионирование
- ✅ Переиспользование в разных проектах
- ✅ Маленький размер Flutter плагина
- ✅ Легкое обновление FFmpeg

## 🚀 Быстрый старт

### 1. Установка зависимостей

```bash
# Ubuntu/Debian
sudo apt install build-essential yasm nasm pkg-config wget

# macOS
brew install yasm nasm pkg-config wget

# Windows - используйте WSL2 (см. WINDOWS_BUILD.md)
```

### 2. Установка Android NDK

Через Android Studio:

- Tools → SDK Manager → SDK Tools
- Отметить "NDK (Side by side)"
- Нажать Apply

Или установить переменную окружения:

```bash
export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/26.x.xxxx
```

### 3. Сборка FFmpeg

```bash
cd smart-ffmpeg-android
chmod +x build_ffmpeg.sh
./build_ffmpeg.sh
```

Это займет 15-25 минут. Скрипт:

- Скачает FFmpeg 6.1
- Соберет для arm64-v8a и armeabi-v7a
- Установит библиотеки в `src/main/jniLibs/`
- Скопирует заголовки в `src/main/cpp/include/`

### 4. Сборка AAR

#### Linux / macOS:

```bash
./gradlew assembleRelease
```

#### Windows:

```cmd
gradlew.bat assembleRelease
```

Результат: `build/outputs/aar/smart-ffmpeg-android-release.aar`

### 5. Проверка

```bash
# Проверить размер (должен быть < 15MB)
ls -lh build/outputs/aar/smart-ffmpeg-android-release.aar

# Проверить содержимое
unzip -l build/outputs/aar/smart-ffmpeg-android-release.aar
```

## 📦 Публикация

### 1. Настройка GitHub Packages

Создайте Personal Access Token:

- GitHub Settings → Developer settings → Personal access tokens
- Создать токен с правами `write:packages`

Добавьте в `~/.gradle/gradle.properties`:

```properties
gpr.user=ВАШ_GITHUB_USERNAME
gpr.key=ВАШ_GITHUB_TOKEN
```

### 2. Обновите URL репозитория

В `build.gradle` замените:

```groovy
url = uri("https://maven.pkg.github.com/YOUR_USERNAME/smart-ffmpeg-android")
```

### 3. Публикация

```bash
./gradlew publish
```

## 🔌 Интеграция с Flutter плагином

### 1. Добавьте репозиторий

В `android/build.gradle` Flutter плагина:

```groovy
allprojects {
    repositories {
        google()
        mavenCentral()

        maven {
            name = "GitHubPackages"
            url = uri("https://maven.pkg.github.com/YOUR_USERNAME/smart-ffmpeg-android")
            credentials {
                username = project.findProperty("gpr.user")
                password = project.findProperty("gpr.key")
            }
        }
    }
}
```

### 2. Добавьте зависимость

В `android/build.gradle` (module level):

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg:1.0.0'
}
```

### 3. Используйте в коде

```kotlin
import com.smartmedia.ffmpeg.SmartFfmpegBridge

// Извлечь обложку
val thumbnail = SmartFfmpegBridge.extractThumbnail(
    videoPath = "/path/to/video.mp4",
    timeMs = 1000L,
    width = 320,
    height = 180
)

// Получить длительность
val duration = SmartFfmpegBridge.getVideoDuration("/path/to/video.mp4")

// Получить метаданные
val metadata = SmartFfmpegBridge.getVideoMetadata("/path/to/video.mp4")
val width = metadata["width"] as Int
val height = metadata["height"] as Int

// Получить версию FFmpeg
val version = SmartFfmpegBridge.getFFmpegVersion()
```

## 📚 Документация

- **README.md** - Обзор (English)
- **README_RU.md** - Этот файл (Русский)
- **QUICKSTART.md** - Быстрый старт
- **BUILDING.md** - Детальная сборка
- **INTEGRATION.md** - Интеграция с Flutter
- **CHECKLIST.md** - Чеклист сборки
- **WINDOWS_BUILD.md** - Сборка на Windows
- **SUMMARY.md** - Полное описание проекта

## 🎯 Конфигурация FFmpeg

### Включено (LGPL-only)

- ✅ H.264 декодер
- ✅ HEVC декодер
- ✅ MPEG4 декодер
- ✅ VP8/VP9 декодеры
- ✅ MP4/MOV демультиплексор
- ✅ Matroska демультиплексор
- ✅ File протокол

### Отключено

- ❌ Все энкодеры
- ❌ Все мультиплексоры
- ❌ Все фильтры
- ❌ Сетевые протоколы
- ❌ GPL кодеки
- ❌ GPL компоненты

## 📊 Размеры

- arm64-v8a: ~7-8MB
- armeabi-v7a: ~6-7MB
- Итого AAR: ~12-15MB

## 🔧 Команды

```bash
# Сборка FFmpeg
./build_ffmpeg.sh

# Сборка AAR
./gradlew assembleRelease

# Очистка
./gradlew clean

# Публикация
./gradlew publish

# Проверка содержимого AAR
unzip -l build/outputs/aar/smart-ffmpeg-android-release.aar

# Проверка размера
du -h build/outputs/aar/smart-ffmpeg-android-release.aar
```

## 🐛 Решение проблем

### NDK не найден

```bash
export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/26.x.xxxx
```

### Ошибка сборки FFmpeg

- Проверьте версию NDK (должна быть r21+)
- Убедитесь что установлены все инструменты сборки
- Проверьте свободное место на диске (~2GB)

### AAR слишком большой

- Проверьте флаги configure в `build_ffmpeg.sh`
- Убедитесь что используется `--enable-small`
- Отключите ненужные кодеки

### Библиотека не загружается

- Проверьте структуру `jniLibs`
- Проверьте пути в CMakeLists.txt
- Убедитесь что собраны оба ABI

## 🎉 Что дальше?

1. ✅ Проект создан
2. 📝 Соберите FFmpeg: `./build_ffmpeg.sh`
3. 📝 Соберите AAR: `./gradlew assembleRelease`
4. 📝 Протестируйте на реальном устройстве
5. 📝 Опубликуйте в GitHub Packages
6. 📝 Интегрируйте с Flutter плагином
7. 📝 Обновите документацию плагина

## 📞 Поддержка

- Issues: GitHub Issues
- Обсуждения: GitHub Discussions
- Wiki: GitHub Wiki

## 📄 Лицензия

LGPL 2.1 или новее (как FFmpeg LGPL сборка)

## 🙏 Благодарности

- FFmpeg Team - https://ffmpeg.org
- Android NDK Team
- Участники проекта

---

**Статус**: ✅ Готов к сборке FFmpeg

**Следующий шаг**: Запустите `./build_ffmpeg.sh`

**Время до продакшена**: ~1-2 часа
