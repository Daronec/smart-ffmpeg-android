# 🚀 Команды для публикации

## Быстрая публикация (копируй и вставляй)

### Шаг 1: Очистка

```cmd
cleanup_before_publish.bat
```

### Шаг 2: Создать репозиторий

Откройте в браузере: https://github.com/new

- Название: `smart-ffmpeg-android`
- Public
- Без README/LICENSE/.gitignore

### Шаг 3: Git команды

```bash
git init
git add .
git commit -m "Initial commit: Smart FFmpeg Android v1.0.0"
git branch -M main
git remote add origin https://github.com/Daronec/smart-ffmpeg-android.git
git push -u origin main
```

### Шаг 4: Создать релиз

Откройте: https://github.com/Daronec/smart-ffmpeg-android/releases/new

- Tag: `v1.0.0`
- Title: `v1.0.0 - Initial Release`
- Описание: из CHANGELOG.md
- Publish release

## Проверка перед push

```bash
# Проверить статус
git status

# Убедиться, что gradle.properties НЕ добавлен
git status | findstr gradle.properties
# Должно быть пусто!

# Проверить что будет закоммичено
git diff --cached
```

## После публикации

### Проверить пакет

https://github.com/Daronec?tab=packages

### Проверить Actions

https://github.com/Daronec/smart-ffmpeg-android/actions

### Добавить topics

```
android
ffmpeg
video-player
kotlin
media-processing
jni
native
video-processing
thumbnail
```

## Использование библиотеки

### settings.gradle

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

### app/build.gradle

```groovy
dependencies {
    implementation 'com.smartmedia:smart-ffmpeg-android:1.0.0'
}
```

### Пользователи должны добавить в ~/.gradle/gradle.properties

```properties
gpr.user=ИХ_GITHUB_USERNAME
gpr.key=ИХ_GITHUB_TOKEN
```

## Troubleshooting

### Ошибка: "remote: Repository not found"

```bash
# Проверить remote
git remote -v

# Исправить если нужно
git remote set-url origin https://github.com/Daronec/smart-ffmpeg-android.git
```

### Ошибка: "failed to push some refs"

```bash
# Принудительный push (только для первого коммита!)
git push -u origin main --force
```

### Ошибка при публикации в GitHub Packages

- Проверьте токен в `~/.gradle/gradle.properties`
- Токен должен иметь права: `write:packages`, `read:packages`
- Проверьте URL в `build.gradle`

## Полезные ссылки

- Репозиторий: https://github.com/Daronec/smart-ffmpeg-android
- Пакеты: https://github.com/Daronec?tab=packages
- Actions: https://github.com/Daronec/smart-ffmpeg-android/actions
- Issues: https://github.com/Daronec/smart-ffmpeg-android/issues
- Releases: https://github.com/Daronec/smart-ffmpeg-android/releases

## Следующие версии

### v1.1.0 (планируется)

- armeabi-v7a поддержка
- Субтитры
- HTTP/RTSP streaming

### v1.2.0 (планируется)

- x86_64 поддержка
- Аппаратное кодирование
- Видео фильтры
