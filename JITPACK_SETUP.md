# 📦 Публикация smart-ffmpeg-android на JitPack

Подробная инструкция по публикации нативной библиотеки на JitPack для устранения необходимости использовать GitHub credentials.

---

## ✅ Что уже сделано

1. ✅ Обновлен `build.gradle` с правильным `groupId = 'com.github.Daronec'`
2. ✅ Создан `jitpack.yml` с конфигурацией Java 11
3. ✅ Удалена конфигурация GitHub Packages
4. ✅ Настроен `maven-publish` плагин

---

## 🚀 Шаги для публикации

### Шаг 1: Создание Git тега

```bash
# Убедитесь, что вы на ветке main
git checkout main

# Создайте тег (БЕЗ префикса v для JitPack)
git tag 1.0.4

# Запушьте тег
git push origin 1.0.4
```

**Важно:** JitPack использует теги без префикса `v`. Используйте `1.0.4`, а не `v1.0.4`.

### Шаг 2: Создание GitHub Release

1. Перейдите: https://github.com/Daronec/smart-ffmpeg-android/releases
2. Нажмите **"Create a new release"**
3. Заполните форму:
   - **Choose a tag:** 1.0.4
   - **Release title:** `1.0.4 - JitPack Release`
   - **Description:**

````markdown
# smart-ffmpeg-android 1.0.4

Android library with FFmpeg 4.4.2 integration for video processing.

## Features

- FFmpeg 4.4.2 with JNI bridge
- Architectures: arm64-v8a
- Methods: extractThumbnail, getVideoDuration, getVideoMetadata, getFFmpegVersion

## Installation via JitPack

Add to your `build.gradle`:

```groovy
repositories {
    maven { url 'https://jitpack.io' }
}

dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.4'
}
```
````

No GitHub credentials required!

## Links

- JitPack: https://jitpack.io/#Daronec/smart-ffmpeg-android
- Documentation: https://github.com/Daronec/smart-ffmpeg-android

````

4. Нажмите **"Publish release"**

### Шаг 3: Запуск сборки на JitPack

1. Перейдите на https://jitpack.io/
2. В поле поиска введите: `Daronec/smart-ffmpeg-android`
3. Нажмите **"Look up"**
4. Найдите версию `1.0.4` в списке
5. Нажмите **"Get it"**

JitPack начнет сборку. Это может занять 5-15 минут.

### Шаг 4: Проверка статуса сборки

- 🟢 **Зеленая галочка** - сборка успешна ✅
- 🔴 **Красный крестик** - ошибка сборки ❌
- 🟡 **Желтый круг** - сборка в процессе ⏳

Если сборка не удалась, нажмите на красный крестик, чтобы увидеть логи.

---

## 📝 Использование библиотеки

### Для пользователей Android

**build.gradle (Project level):**

```groovy
allprojects {
    repositories {
        google()
        mavenCentral()
        maven { url 'https://jitpack.io' }
    }
}
````

**build.gradle (App level):**

```groovy
dependencies {
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.4'
}
```

### Для Flutter плагинов

**android/build.gradle:**

```groovy
repositories {
    google()
    mavenCentral()
    maven { url 'https://jitpack.io' }
}

dependencies {
    implementation 'org.jetbrains.kotlin:kotlin-stdlib:1.9.0'
    implementation 'com.github.Daronec:smart-ffmpeg-android:1.0.4'
}
```

---

## 🔍 Проверка работы

### Добавьте badge в README

```markdown
[![](https://jitpack.io/v/Daronec/smart-ffmpeg-android.svg)](https://jitpack.io/#Daronec/smart-ffmpeg-android)
```

### Проверьте статус библиотеки

1. Перейдите: https://jitpack.io/#Daronec/smart-ffmpeg-android
2. Убедитесь, что версия 1.0.4 имеет зеленую галочку ✅
3. Нажмите на версию, чтобы увидеть детали сборки

---

## 🆘 Устранение проблем

### Проблема: "Could not find build.gradle"

**Решение:** Убедитесь, что `build.gradle` находится в корне репозитория

### Проблема: "Task 'install' not found"

**Решение:** Проверьте, что в `jitpack.yml` указана правильная команда сборки

### Проблема: "NDK not found"

**Решение:** Убедитесь, что `.so` файлы находятся в `src/main/jniLibs/`

### Проблема: "Java version mismatch"

**Решение:** Проверьте `jitpack.yml` - должна быть указана Java 11

### Проблема: Старая версия кэшируется

**Решение:**

```bash
./gradlew clean
rm -rf ~/.gradle/caches/
./gradlew build --refresh-dependencies
```

---

## 📊 Преимущества JitPack

### Для пользователей:

✅ **Простая установка** - одна строка в `build.gradle`
✅ **Нет credentials** - не нужен GitHub токен
✅ **Быстрая сборка** - JitPack кэширует собранные библиотеки

### Для разработчиков:

✅ **Меньше поддержки** - нет вопросов про GitHub credentials
✅ **Стандартный подход** - JitPack знаком большинству Android разработчиков
✅ **Простое обновление** - создайте новый тег, JitPack автоматически соберет

---

## 📚 Дополнительные ресурсы

- **JitPack Docs:** https://jitpack.io/docs/
- **JitPack Building:** https://jitpack.io/docs/BUILDING/
- **Android Library Guide:** https://developer.android.com/studio/projects/android-library

---

## ✨ Готово!

После выполнения всех шагов:

✅ Библиотека доступна на JitPack
✅ Пользователям не нужны GitHub credentials
✅ Установка максимально простая

**Поздравляю! Теперь ваша библиотека еще проще в использовании!** 🎉
