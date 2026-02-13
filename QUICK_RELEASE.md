# Quick Release Guide

## 🚀 Самый быстрый способ

```bash
# 1. Запустите скрипт релиза
chmod +x release.sh
./release.sh 1.0.6

# 2. Следуйте инструкциям
# Скрипт автоматически:
# ✅ Обновит версию в build.gradle
# ✅ Запустит тесты
# ✅ Соберет библиотеку
# ✅ Создаст коммит
# ✅ Создаст тег
# ✅ Запушит в GitHub

# 3. Готово! 🎉
```

## 📋 Ручной способ

```bash
# 1. Обновите версию
vim build.gradle  # version = '1.0.6'

# 2. Тесты и сборка
./gradlew test
./gradlew assembleRelease

# 3. Коммит и тег
git add build.gradle
git commit -m "Release 1.0.6"
git tag 1.0.6

# 4. Пуш
git push origin main
git push origin 1.0.6
```

## ⏱️ Что происходит дальше?

После push тега GitHub Actions автоматически (~5 минут):

1. ✅ Соберет библиотеку
2. ✅ Опубликует в GitHub Packages
3. ✅ Создаст GitHub Release
4. ✅ Триггернет JitPack build

## 🔗 Проверка статуса

- **GitHub Actions:** https://github.com/Daronec/smart-ffmpeg-android/actions
- **GitHub Release:** https://github.com/Daronec/smart-ffmpeg-android/releases
- **JitPack:** https://jitpack.io/#Daronec/smart-ffmpeg-android

## 📦 Использование

После успешного релиза пользователи могут установить:

```groovy
repositories {
    maven { url 'https://jitpack.io' }
}

dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.6'
}
```

## 🐛 Если что-то пошло не так

### Отменить релиз (до push)

```bash
# Удалить тег локально
git tag -d 1.0.6

# Откатить коммит
git reset --hard HEAD~1
```

### Удалить релиз (после push)

```bash
# Удалить тег удаленно
git push origin :refs/tags/1.0.6

# Удалить тег локально
git tag -d 1.0.6

# Удалить Release на GitHub вручную
# https://github.com/Daronec/smart-ffmpeg-android/releases
```

---

Подробная документация: [RELEASE_WORKFLOW.md](RELEASE_WORKFLOW.md)
