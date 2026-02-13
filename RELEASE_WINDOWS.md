# Release Guide for Windows

## 🚀 Быстрый релиз на Windows

### Вариант 1: PowerShell скрипт (рекомендуется)

```powershell
# Запустить PowerShell скрипт
.\release.ps1 1.0.5

# Скрипт автоматически:
# ✅ Обновит версию в build.gradle
# ✅ Соберет библиотеку
# ✅ Создаст коммит и тег
# ✅ Запушит в GitHub
```

### Вариант 2: Ручной релиз (если скрипт не работает)

```powershell
# 1. Обновить версию в build.gradle вручную
# Открыть build.gradle и изменить:
# version = '1.0.5'

# 2. Собрать библиотеку (опционально, для проверки)
.\gradlew.bat assembleRelease

# 3. Коммит и тег
git add build.gradle
git commit -m "Release 1.0.5"
git tag 1.0.5

# 4. Пуш
git push origin main
git push origin 1.0.5
```

### Вариант 3: Только тег (самый простой)

Если версия уже обновлена в build.gradle:

```powershell
# Создать и запушить тег
git tag 1.0.5
git push origin 1.0.5
```

GitHub Actions автоматически обновит версию в build.gradle при сборке.

## ⚠️ Важно для Windows

### Проблема: JAVA_HOME не настроен

Если видите ошибку:

```
ERROR: JAVA_HOME is not set
```

**Решение 1: Пропустить тесты локально**

Тесты будут запущены автоматически в GitHub Actions. Просто создайте тег:

```powershell
git tag 1.0.5
git push origin 1.0.5
```

**Решение 2: Настроить JAVA_HOME**

```powershell
# Найти путь к Java
where java

# Установить JAVA_HOME (пример)
$env:JAVA_HOME = "C:\Program Files\Java\jdk-17"

# Или добавить в System Environment Variables
# Control Panel → System → Advanced → Environment Variables
# Добавить JAVA_HOME = C:\Program Files\Java\jdk-17
```

**Решение 3: Использовать Android Studio Terminal**

Android Studio уже настроил JAVA_HOME:

1. Открыть Android Studio
2. Открыть Terminal (Alt+F12)
3. Запустить:
   ```bash
   bash release.sh 1.0.5
   ```

## 📋 Checklist перед релизом

- [ ] Версия обновлена в `build.gradle`
- [ ] Все изменения закоммичены
- [ ] Тег создан: `git tag 1.0.5`
- [ ] Тег запушен: `git push origin 1.0.5`

## 🎯 После push тега

GitHub Actions автоматически (~5 минут):

1. ✅ Соберет библиотеку
2. ✅ Запустит тесты
3. ✅ Опубликует в GitHub Packages
4. ✅ Создаст GitHub Release
5. ✅ Триггернет JitPack build

## 🔗 Проверка статуса

```powershell
# Открыть в браузере
start https://github.com/Daronec/smart-ffmpeg-android/actions
start https://jitpack.io/#Daronec/smart-ffmpeg-android/1.0.5
```

## 🐛 Troubleshooting

### Скрипт не запускается

```powershell
# Разрешить выполнение скриптов
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser

# Запустить снова
.\release.ps1 1.0.5
```

### Git команды не работают

Убедитесь, что Git установлен и добавлен в PATH:

```powershell
git --version
```

Если нет, установите Git: https://git-scm.com/download/win

### Gradle не найден

```powershell
# Использовать wrapper
.\gradlew.bat assembleRelease

# Не используйте просто "gradle"
```

## 💡 Рекомендации

1. **Используйте PowerShell скрипт** - он пропускает тесты на Windows
2. **Или просто создайте тег** - GitHub Actions сделает всё остальное
3. **Не запускайте тесты локально** - они требуют Android SDK и эмулятор

## 🎉 Пример успешного релиза

```powershell
PS C:\Work\smart-ffmpeg-android> .\release.ps1 1.0.5

ℹ️  Starting release process for version 1.0.5
✅ Version updated in build.gradle
⚠️  Skipping tests on Windows (will run in GitHub Actions)
✅ Library built successfully
✅ Changes committed
✅ Tag created
✅ Commit pushed
✅ Tag pushed
🎉 Release 1.0.5 initiated!

ℹ️  Next steps:
  1. Check GitHub Actions: https://github.com/Daronec/smart-ffmpeg-android/actions
  2. Wait for workflow to complete (~5 minutes)
  3. Check GitHub Release: https://github.com/Daronec/smart-ffmpeg-android/releases/tag/1.0.5
  4. Check JitPack: https://jitpack.io/#Daronec/smart-ffmpeg-android/1.0.5
```

---

**Для Linux/Mac:** Используйте `release.sh` вместо `release.ps1`
