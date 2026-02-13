# Release Workflow Guide

Автоматизированный процесс публикации библиотеки Smart FFmpeg Android.

## 🚀 Быстрый старт

### Автоматический релиз (рекомендуется)

1. **Обновите версию в `build.gradle`:**

   ```groovy
   version = '1.0.6'  // Новая версия
   ```

2. **Закоммитьте изменения:**

   ```bash
   git add build.gradle
   git commit -m "Bump version to 1.0.6"
   git push origin main
   ```

3. **Создайте и запушьте тег:**

   ```bash
   git tag 1.0.6
   git push origin 1.0.6
   ```

4. **Готово!** GitHub Actions автоматически:
   - ✅ Соберет библиотеку
   - ✅ Запустит тесты
   - ✅ Опубликует в GitHub Packages
   - ✅ Создаст GitHub Release с артефактами
   - ✅ Триггернет JitPack build

### Ручной релиз

Если нужно запустить релиз вручную:

1. Перейдите в **Actions** → **Release & Publish**
2. Нажмите **Run workflow**
3. Введите версию (например, `1.0.6`)
4. Нажмите **Run workflow**

## 📋 Что происходит при релизе

### 1. Build & Test

```
✓ Checkout code
✓ Setup JDK 17
✓ Setup Android SDK
✓ Run tests
✓ Build library (AAR)
✓ Generate sources JAR
```

### 2. Publish to GitHub Packages

```
✓ Publish AAR to GitHub Packages
✓ Publish POM with metadata
✓ Publish sources JAR
```

**Доступ:**

```groovy
repositories {
    maven {
        url = uri("https://maven.pkg.github.com/Daronec/smart-ffmpeg-android")
        credentials {
            username = "YOUR_GITHUB_USERNAME"
            password = "YOUR_GITHUB_TOKEN"
        }
    }
}

dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.6'
}
```

### 3. Create GitHub Release

```
✓ Generate changelog from commits
✓ Create release with tag
✓ Upload AAR file
✓ Upload sources JAR
✓ Add release notes
```

**Результат:**

- 📦 Release page: `https://github.com/Daronec/smart-ffmpeg-android/releases/tag/1.0.6`
- 📥 Downloadable artifacts (AAR, sources)
- 📝 Автоматический changelog

### 4. Trigger JitPack Build

```
✓ Call JitPack API
✓ Trigger build for new version
✓ Wait for JitPack to process
```

**Доступ (без credentials!):**

```groovy
repositories {
    maven { url 'https://jitpack.io' }
}

dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.6'
}
```

**Проверка статуса:**

- 🔗 https://jitpack.io/#Daronec/smart-ffmpeg-android/1.0.6

## 🔧 Настройка

### Требования

1. **GitHub Token** (автоматически доступен в Actions)
   - Используется для публикации в GitHub Packages
   - Используется для создания Release

2. **JitPack** (не требует настройки)
   - Автоматически обнаруживает новые теги
   - Собирает библиотеку по запросу

### Переменные окружения

GitHub Actions автоматически предоставляет:

- `GITHUB_TOKEN` - для публикации
- `GITHUB_ACTOR` - username для GitHub Packages

## 📝 Версионирование

Используем [Semantic Versioning](https://semver.org/):

```
MAJOR.MINOR.PATCH

1.0.5
│ │ │
│ │ └─ Patch: Bug fixes, small improvements
│ └─── Minor: New features, backward compatible
└───── Major: Breaking changes
```

### Примеры:

- `1.0.5` → `1.0.6` - Bug fix
- `1.0.6` → `1.1.0` - New feature (backward compatible)
- `1.1.0` → `2.0.0` - Breaking change

## 🎯 Checklist перед релизом

- [ ] Все тесты проходят локально (`./gradlew test`)
- [ ] Обновлена версия в `build.gradle`
- [ ] Обновлен `CHANGELOG.md` (опционально)
- [ ] Обновлена документация (если есть изменения API)
- [ ] Создан коммит с изменениями
- [ ] Создан и запушен тег

## 🐛 Troubleshooting

### Проблема: JitPack build failed

**Решение:**

1. Проверьте логи: https://jitpack.io/#Daronec/smart-ffmpeg-android
2. Убедитесь, что `jitpack.yml` корректен
3. Проверьте, что тег существует: `git tag -l`
4. Попробуйте пересобрать: нажмите "Get it" на JitPack

### Проблема: GitHub Packages authentication failed

**Решение:**

1. Проверьте, что `GITHUB_TOKEN` доступен в Actions
2. Убедитесь, что у workflow есть `permissions: packages: write`
3. Проверьте credentials в `build.gradle`

### Проблема: Release не создался

**Решение:**

1. Проверьте, что у workflow есть `permissions: contents: write`
2. Убедитесь, что тег существует
3. Проверьте логи GitHub Actions

## 📚 Дополнительные ресурсы

- [GitHub Packages Documentation](https://docs.github.com/en/packages)
- [JitPack Documentation](https://jitpack.io/docs/)
- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [Semantic Versioning](https://semver.org/)

## 🔄 Workflow файлы

### `.github/workflows/release.yml`

Основной workflow для релиза. Триггерится при создании тега.

### `.github/workflows/build.yml`

CI workflow для проверки сборки на каждом push/PR.

### `.github/workflows/publish.yml`

Legacy workflow для публикации (можно удалить, заменен на `release.yml`).

## 📊 Статус релиза

После запуска workflow вы увидите:

```
✅ Build & Test
✅ Publish to GitHub Packages
✅ Create GitHub Release
✅ Trigger JitPack Build
✅ Post-release notifications
```

Комментарий будет автоматически добавлен к коммиту с инструкциями по установке.

## 🎉 Пример успешного релиза

```bash
# 1. Обновить версию
vim build.gradle  # version = '1.0.6'

# 2. Коммит
git add build.gradle
git commit -m "Release 1.0.6: Add new metadata fields"

# 3. Тег
git tag 1.0.6
git push origin main
git push origin 1.0.6

# 4. Ждем ~5 минут
# ✅ GitHub Release создан
# ✅ GitHub Packages обновлен
# ✅ JitPack собрал пакет

# 5. Проверяем
curl https://jitpack.io/api/builds/com.github.Daronec/smart-ffmpeg-android/1.0.6
```

---

**Версия документа:** 1.0  
**Дата:** 2026-02-13  
**Автор:** Daronec
