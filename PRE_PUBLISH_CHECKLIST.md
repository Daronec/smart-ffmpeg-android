# Чеклист перед публикацией на GitHub

## ✅ Что уже готово

### Структура проекта

- ✅ Правильная структура `src/main/`
- ✅ Нативный движок в `src/main/cpp/native_media_engine/`
- ✅ Kotlin API в `src/main/kotlin/`
- ✅ CMakeLists.txt настроен
- ✅ build.gradle настроен с GitHub Packages
- ✅ settings.gradle настроен

### FFmpeg библиотеки

- ✅ arm64-v8a библиотеки присутствуют (7 файлов)
- ✅ Заголовки FFmpeg в `include/`
- ✅ Исходники плеера (50 файлов)

### Безопасность

- ✅ `.gitignore` настроен правильно
- ✅ `gradle.properties` в корне НЕ содержит токенов
- ✅ Создан `SECURITY.md`
- ✅ Создан `FIX_TOKEN_LEAK.md`

### Документация

- ✅ README.md
- ✅ GITHUB_README.md (для GitHub)
- ✅ STRUCTURE.md
- ✅ INTEGRATION_GUIDE.md
- ✅ PUBLISH.md
- ✅ FIRST_COMMIT.md
- ✅ LICENSE (LGPL 2.1)

### CI/CD

- ✅ `.github/workflows/build.yml`
- ✅ `.github/workflows/publish.yml`

## ⚠️ Что нужно сделать ПЕРЕД публикацией

### 1. Проверить токен ✅

Токен уже настроен в `C:\Users\YOUR_USERNAME\.gradle\gradle.properties`

**Проверка:**

```cmd
type %USERPROFILE%\.gradle\gradle.properties
```

Должно быть:

```properties
gpr.user=Daronec
gpr.key=ghp_...
```

⚠️ **Важно:** Токен НЕ должен быть в папке проекта! Только в `~/.gradle/gradle.properties`

### 2. Добавить armeabi-v7a библиотеки (опционально)

- [ ] Собрать FFmpeg для armeabi-v7a
- [ ] Скопировать в `src/main/cpp/native_media_engine/jniLibs/armeabi-v7a/`
- [ ] Или удалить `armeabi-v7a` из `build.gradle` если не нужна поддержка 32-bit

### 3. Проверить build.gradle

- [ ] Версия: `1.0.0`
- [ ] groupId: `com.smartmedia`
- [ ] artifactId: `smart-ffmpeg-android`
- [ ] URL: `https://maven.pkg.github.com/Daronec/smart-ffmpeg-android`

### 4. Очистить проект

- [ ] Удалить временные файлы:
  - [ ] `nul`
  - [ ] `CURRENT_SITUATION.txt`
  - [ ] `FIX_NDK_ERROR.txt`
  - [ ] `QUICK_FIX.txt`
  - [ ] `COMPLETION_REPORT.md`
  - [ ] `START_HERE_WINDOWS.txt`
  - [ ] Другие временные файлы

### 5. Тестирование (ВАЖНО!)

- [ ] Собрать проект: `./gradlew clean assembleRelease`
- [ ] Проверить размер AAR (должен быть ~10-15MB)
- [ ] Создать тестовое Android приложение
- [ ] Протестировать `SmartFFmpegPlayer`
- [ ] Протестировать `SmartFfmpegBridge`
- [ ] Проверить на реальном устройстве

### 6. Подготовить README для GitHub

- [ ] Переименовать `GITHUB_README.md` → `README.md` (или объединить)
- [ ] Добавить badges (build status, version)
- [ ] Добавить скриншоты/примеры (опционально)

### 7. Создать CHANGELOG.md

- [ ] Создать файл `CHANGELOG.md`
- [ ] Добавить версию 1.0.0 с описанием функций

## 📋 Пошаговая инструкция

### Шаг 1: Исправить токен

```bash
# 1. Удалите старый токен на https://github.com/settings/tokens
# 2. Создайте новый токен
# 3. Сохраните в C:\Users\YOUR_USERNAME\.gradle\gradle.properties:
gpr.user=Daronec
gpr.key=YOUR_NEW_TOKEN
```

### Шаг 2: Очистить проект

```bash
# Удалите временные файлы
del nul
del CURRENT_SITUATION.txt
del FIX_NDK_ERROR.txt
del QUICK_FIX.txt
del COMPLETION_REPORT.md
del START_HERE_WINDOWS.txt
```

### Шаг 3: Обновить build.gradle (если нужно)

```groovy
// Если не нужна поддержка 32-bit, удалите armeabi-v7a:
ndk {
    abiFilters 'arm64-v8a'  // Только 64-bit
}
```

### Шаг 4: Создать CHANGELOG.md

```markdown
# Changelog

## [1.0.0] - 2024-XX-XX

### Added

- Video playback with FFmpeg
- Thumbnail extraction
- Video metadata extraction
- Hardware acceleration support
- Audio/Video synchronization
- Frame-accurate seeking
- Playback speed control (0.5x - 3.0x)

### Supported Formats

- MP4, AVI, FLV, MKV, WebM, and more

### Supported Architectures

- arm64-v8a (64-bit)
```

### Шаг 5: Тестовая сборка

```bash
# Очистить
./gradlew clean

# Собрать
./gradlew assembleRelease

# Проверить результат
dir build\outputs\aar\
```

### Шаг 6: Создать репозиторий на GitHub

1. Перейдите на https://github.com/new
2. Название: `smart-ffmpeg-android`
3. Описание: `Android library for video playback and media processing using FFmpeg`
4. Public
5. НЕ добавляйте README, .gitignore, LICENSE
6. Create repository

### Шаг 7: Первый коммит

```bash
git init
git add .
git commit -m "Initial commit: Smart FFmpeg Android library v1.0.0"
git branch -M main
git remote add origin https://github.com/Daronec/smart-ffmpeg-android.git
git push -u origin main
```

### Шаг 8: Создать релиз

1. https://github.com/Daronec/smart-ffmpeg-android/releases/new
2. Tag: `v1.0.0`
3. Title: `v1.0.0 - Initial Release`
4. Описание из CHANGELOG.md
5. Publish release

### Шаг 9: Проверить публикацию

- GitHub Actions должен автоматически собрать и опубликовать пакет
- Проверьте: https://github.com/Daronec?tab=packages

## ⚡ Быстрый чеклист

Перед `git push`:

- [ ] Токен удален и пересоздан
- [ ] Токен сохранен в `~/.gradle/gradle.properties`
- [ ] Проект собирается: `./gradlew assembleRelease`
- [ ] Временные файлы удалены
- [ ] CHANGELOG.md создан
- [ ] README.md актуален
- [ ] `.gitignore` проверен

## 🚨 Критические проверки

Перед публикацией убедитесь:

- ❌ НЕТ токенов в проекте
- ❌ НЕТ паролей в проекте
- ❌ НЕТ личных данных в коде
- ✅ Все пути относительные
- ✅ Лицензия LGPL 2.1
- ✅ FFmpeg attribution присутствует

## 📞 Если что-то пошло не так

1. Проверьте `SECURITY.md`
2. Проверьте `FIX_TOKEN_LEAK.md`
3. Проверьте `FIRST_COMMIT.md`
4. Проверьте логи: `./gradlew build --stacktrace`

## ✨ После публикации

- [ ] Проверить пакет на GitHub Packages
- [ ] Создать тестовое приложение
- [ ] Протестировать установку из GitHub Packages
- [ ] Обновить документацию (если нужно)
- [ ] Анонсировать релиз

---

**Следующий файл для чтения:** `FIRST_COMMIT.md`
