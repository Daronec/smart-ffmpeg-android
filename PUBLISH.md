# Публикация библиотеки в GitHub Packages

## Настройка GitHub Token

1. Перейдите на GitHub: https://github.com/settings/tokens
2. Нажмите "Generate new token" → "Generate new token (classic)"
3. Выберите scopes:
   - ✅ `write:packages` (для публикации)
   - ✅ `read:packages` (для чтения)
4. Скопируйте созданный token

## Настройка локальных credentials

Создайте файл `gradle.properties` в домашней директории (`~/.gradle/gradle.properties`):

```properties
gpr.user=Daronec
gpr.key=YOUR_GITHUB_TOKEN
```

Или установите переменные окружения:

```bash
# Windows (CMD)
set GPR_USER=Daronec
set GPR_KEY=YOUR_GITHUB_TOKEN

# Windows (PowerShell)
$env:GPR_USER="Daronec"
$env:GPR_KEY="YOUR_GITHUB_TOKEN"

# Linux/Mac
export GPR_USER=Daronec
export GPR_KEY=YOUR_GITHUB_TOKEN
```

## Публикация

```bash
# Собрать и опубликовать
./gradlew publish

# Или только release версию
./gradlew publishReleasePublicationToGitHubPackagesRepository
```

## Использование библиотеки

После публикации другие проекты смогут использовать библиотеку:

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

## GitHub Actions (автоматическая публикация)

Создайте файл `.github/workflows/publish.yml`:

```yaml
name: Publish to GitHub Packages

on:
  release:
    types: [created]

jobs:
  publish:
    runs-on: ubuntu-latest
    permissions:
      contents: read
      packages: write

    steps:
      - uses: actions/checkout@v3

      - name: Set up JDK 11
        uses: actions/setup-java@v3
        with:
          java-version: "11"
          distribution: "temurin"

      - name: Setup Android SDK
        uses: android-actions/setup-android@v2

      - name: Grant execute permission for gradlew
        run: chmod +x gradlew

      - name: Build and publish
        run: ./gradlew publish
        env:
          GPR_USER: ${{ github.actor }}
          GPR_KEY: ${{ secrets.GITHUB_TOKEN }}
```

## Информация о пакете

- **Group ID**: `com.smartmedia`
- **Artifact ID**: `smart-ffmpeg-android`
- **Version**: `1.0.0`
- **Repository**: https://github.com/Daronec/smart-ffmpeg-android
- **Packages URL**: https://github.com/Daronec?tab=packages

## Проверка публикации

После публикации пакет будет доступен по адресу:
https://github.com/Daronec/smart-ffmpeg-android/packages

## Troubleshooting

### Ошибка 401 Unauthorized

- Проверьте, что token имеет права `write:packages`
- Проверьте, что credentials настроены правильно

### Ошибка 403 Forbidden

- Убедитесь, что репозиторий существует
- Проверьте права доступа к репозиторию

### Ошибка при сборке

```bash
# Очистить кэш
./gradlew clean

# Пересобрать
./gradlew build

# Опубликовать
./gradlew publish
```
