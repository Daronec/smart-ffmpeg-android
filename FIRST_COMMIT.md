# Первый коммит в GitHub

## Шаг 1: Создайте репозиторий на GitHub

1. Перейдите на https://github.com/new
2. Название репозитория: `smart-ffmpeg-android`
3. Описание: `Android library for video playback and media processing using FFmpeg`
4. Выберите: Public или Private
5. НЕ добавляйте README, .gitignore или LICENSE (они уже есть)
6. Нажмите "Create repository"

## Шаг 2: Инициализируйте Git локально

```bash
# Перейдите в папку проекта
cd c:\Work\smart_video_thumbnail\smart-ffmpeg-android

# Инициализируйте Git (если еще не инициализирован)
git init

# Добавьте все файлы
git add .

# Создайте первый коммит
git commit -m "Initial commit: Smart FFmpeg Android library"

# Добавьте remote
git remote add origin https://github.com/Daronec/smart-ffmpeg-android.git

# Отправьте код
git branch -M main
git push -u origin main
```

## Шаг 3: Проверьте GitHub Actions

После push GitHub Actions автоматически запустит сборку:

- Перейдите на https://github.com/Daronec/smart-ffmpeg-android/actions
- Проверьте статус сборки

## Шаг 4: Создайте первый релиз

1. Перейдите на https://github.com/Daronec/smart-ffmpeg-android/releases
2. Нажмите "Create a new release"
3. Tag version: `v1.0.0`
4. Release title: `v1.0.0 - Initial Release`
5. Описание:

   ```markdown
   ## Features

   - Video playback with FFmpeg
   - Thumbnail extraction
   - Video metadata extraction
   - Hardware acceleration support
   - Audio/Video synchronization
   - Frame-accurate seeking
   - Playback speed control

   ## Supported Formats

   MP4, AVI, FLV, MKV, WebM, and more

   ## Requirements

   - Android API 21+
   - arm64-v8a, armeabi-v7a
   ```

6. Нажмите "Publish release"

После публикации релиза GitHub Actions автоматически опубликует пакет в GitHub Packages.

## Шаг 5: Проверьте публикацию

Пакет будет доступен по адресу:
https://github.com/Daronec?tab=packages

## Альтернатива: Используйте GitHub Desktop

Если предпочитаете GUI:

1. Скачайте GitHub Desktop: https://desktop.github.com/
2. Откройте GitHub Desktop
3. File → Add Local Repository
4. Выберите папку проекта
5. Publish repository
6. Выберите имя: `smart-ffmpeg-android`
7. Нажмите "Publish repository"

## Что дальше?

После публикации:

1. Обновите README.md на GitHub (замените GITHUB_README.md → README.md)
2. Добавьте badges (build status, version, license)
3. Создайте Wiki с документацией
4. Настройте Issues и Discussions

## Troubleshooting

### Ошибка: remote origin already exists

```bash
git remote remove origin
git remote add origin https://github.com/Daronec/smart-ffmpeg-android.git
```

### Ошибка: failed to push some refs

```bash
git pull origin main --rebase
git push -u origin main
```

### Большой размер репозитория

Если репозиторий слишком большой из-за .so файлов, используйте Git LFS:

```bash
git lfs install
git lfs track "*.so"
git add .gitattributes
git commit -m "Add Git LFS tracking"
```
